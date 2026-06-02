// Standalone WebSocket signaling server.
//
// Speaks the project signaling protocol over a single WebSocket route ("/*").
// Every frame is one JSON object:
//
//   { "type": "join"|"offer"|"answer"|"ice"|"leave",
//     "room": "<room name>",        // required on join; implied afterwards
//     "from": <id>,                 // filled in by the server on relay
//     "to":   <id>,                 // optional; 0 / absent => broadcast in room
//     "payload": <any> }            // SDP / candidate body, opaque to server
//
// Server -> client notifications:
//   { "type":"joined",      "room", "id", "peers":[...] }   // your id + roster
//   { "type":"peer-joined", "room", "from":<id> }           // someone arrived
//   { "type":"peer-left",   "room", "from":<id> }           // someone left
//   { "type":"error",       "reason":"<text>" }             // bad request
//
// Routing is delegated to RoomManager; this file only owns the transport.
//
// uWebSockets drives all callbacks on one thread-local event loop, so the
// RoomManager (which assumes single-threaded access) is safe here.

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <uwebsockets/App.h>

#include "RoomManager.h"

namespace {

using nlohmann::json;
using vc::signaling::ConnectionHandle;
using vc::signaling::ParticipantId;
using vc::signaling::RoomManager;

// Per-socket user data carried by uWebSockets. We keep nothing here beyond a
// marker; the connection identity is the WebSocket pointer itself, which we use
// as the RoomManager ConnectionHandle.
struct PerSocketData {};

using WsApp = uWS::App;
using Ws = uWS::WebSocket<false, true, PerSocketData>;

// Send a small JSON error frame back to a single socket.
void sendError(Ws* ws, std::string_view reason) {
    json e = {{"type", "error"}, {"reason", std::string(reason)}};
    ws->send(e.dump(), uWS::OpCode::TEXT);
}

}  // namespace

int main(int argc, char** argv) {
    int port = 8080;
    if (argc > 1) {
        port = std::atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid port: " << argv[1] << "\n";
            return 2;
        }
    }

    // RoomManager's SendFn maps an opaque ConnectionHandle back to a WebSocket.
    // Safe because every callback runs on the same loop thread.
    auto manager = std::make_shared<RoomManager>(
        [](ConnectionHandle conn, std::string_view payload) {
            auto* ws = static_cast<Ws*>(conn);
            ws->send(payload, uWS::OpCode::TEXT);
        });

    WsApp app;

    app.ws<PerSocketData>("/*", {
        .compression = uWS::DISABLED,
        .maxPayloadLength = 64 * 1024,
        .idleTimeout = 120,
        .open = [](Ws* /*ws*/) {
            // Nothing to do until the client sends a "join".
        },
        .message = [manager](Ws* ws, std::string_view msg, uWS::OpCode op) {
            if (op != uWS::OpCode::TEXT) {
                sendError(ws, "only text frames are accepted");
                return;
            }
            json j = json::parse(msg, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded() || !j.is_object()) {
                sendError(ws, "malformed JSON");
                return;
            }
            const std::string type = j.value("type", std::string{});
            auto* conn = static_cast<ConnectionHandle>(ws);

            if (type == "join") {
                const std::string room = j.value("room", std::string{});
                auto r = manager->join(room, conn);
                if (!r) {
                    sendError(ws, r.error() == vc::Error::ResourceExhausted
                                      ? "room is full"
                                      : "invalid join request");
                }
                return;
            }

            if (type == "leave") {
                (void)manager->leave(conn);
                return;
            }

            if (type == "offer" || type == "answer" || type == "ice") {
                const ParticipantId fromId = manager->idFor(conn);
                if (fromId == 0) {
                    sendError(ws, "must join a room before signaling");
                    return;
                }
                const ParticipantId to =
                    j.value("to", static_cast<ParticipantId>(0));
                // Stamp the authoritative sender id before forwarding so peers
                // can trust `from` regardless of what the client claimed.
                j["from"] = fromId;
                const std::string forwarded = j.dump();
                auto s = manager->relay(conn, type, to, forwarded);
                if (!s) sendError(ws, "relay target not found");
                return;
            }

            sendError(ws, "unknown message type");
        },
        .close = [manager](Ws* ws, int /*code*/, std::string_view /*message*/) {
            (void)manager->leave(static_cast<ConnectionHandle>(ws));
        },
    });

    app.listen(port, [port](us_listen_socket_t* listenSocket) {
        if (listenSocket) {
            std::cout << "Signaling server listening on ws://0.0.0.0:" << port
                      << "\n";
        } else {
            std::cerr << "Failed to listen on port " << port << "\n";
            std::exit(1);
        }
    });

    app.run();
    return 0;
}
