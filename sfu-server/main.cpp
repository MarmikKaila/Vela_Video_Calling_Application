// Integrated SFU + signaling server.
//
// Unlike the pure-relay signaling-server (which forwards offers/answers between
// two clients), here the SERVER is the media hub: each client establishes ONE
// ICE connection to this process, sends its own audio+video RTP up it, and
// receives every other room member's RTP back down it. The transport-agnostic
// SFUServer (src/sfu) does the verbatim forwarding; an RtpTransport (libjuice
// ICE) per client is the wire.
//
// Signaling protocol (same JSON envelope as signaling-server), but the server
// answers offers itself instead of relaying:
//   client -> { "type":"join",  "room":"<name>" }
//   client -> { "type":"offer", "payload":"<client ICE description>" }
//   client -> { "type":"ice",   "payload":"<client ICE candidate>" }
//   client -> { "type":"leave" }
//   server -> { "type":"joined","room":"<name>","id":"<id>" }
//   server -> { "type":"answer","payload":"<server ICE description>" }
//   server -> { "type":"ice",   "payload":"<server ICE candidate>" }
//
// THREADING. uWebSockets runs on one loop thread and is not thread-safe.
// libjuice invokes RtpTransport callbacks on its own internal thread, so any
// ws->send from those callbacks is marshaled back onto the loop with
// Loop::defer. The high-rate onPacket callback does NOT touch the socket — it
// calls SFUServer::routePacket directly (the SFU and its Rooms are internally
// synchronized), so media forwarding never bottlenecks on the loop thread.

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <uwebsockets/App.h>

#include "network/RtpPacket.h"
#include "sfu/SFUServer.h"
#include "signaling/RtpTransport.h"

namespace {

using nlohmann::json;
using vc::RtpPacket;
using vc::SFUServer;
using vc::signaling::IceState;
using vc::signaling::NATConfig;
using vc::signaling::RtpTransport;

struct PerSocketData;
using WsApp = uWS::App;
using Ws = uWS::WebSocket<false, true, PerSocketData>;

// Per-connection media + identity state. Owned solely by its socket's
// PerSocketData (one shared_ptr). All shared_ptr handling happens on the loop
// thread; libjuice callbacks only hold a weak_ptr and defer to the loop.
struct ClientState {
    std::string id;
    std::string room;
    std::unique_ptr<RtpTransport> transport;
    Ws* ws = nullptr;
    std::atomic<bool> alive{true};
};

struct PerSocketData {
    std::shared_ptr<ClientState> state;
};

void sendJson(Ws* ws, const json& j) { ws->send(j.dump(), uWS::OpCode::TEXT); }

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

    auto sfu = std::make_shared<SFUServer>();
    auto nextId = std::make_shared<std::uint32_t>(1);
    uWS::Loop* loop = uWS::Loop::get();  // this thread runs app.run()

    NATConfig iceCfg;
    iceCfg.stunHost = "";  // LAN: host candidates only, no STUN

    WsApp app;
    app.ws<PerSocketData>("/*", {
        .compression = uWS::DISABLED,
        .maxPayloadLength = 64 * 1024,
        .idleTimeout = 120,
        .open = [](Ws*) {},  // wait for "join"
        .message = [sfu, nextId, loop, iceCfg](Ws* ws, std::string_view msg, uWS::OpCode op) {
            if (op != uWS::OpCode::TEXT) return;
            json j = json::parse(msg, nullptr, /*allow_exceptions=*/false);
            if (j.is_discarded() || !j.is_object()) return;
            const std::string type = j.value("type", std::string{});
            auto* psd = static_cast<PerSocketData*>(ws->getUserData());

            if (type == "join") {
                if (psd->state) return;  // already joined
                auto st = std::make_shared<ClientState>();
                const std::uint32_t numId = (*nextId)++;
                st->id = std::to_string(numId);
                st->room = j.value("room", std::string{});
                st->ws = ws;
                st->transport = std::make_unique<RtpTransport>();
                if (st->room.empty()) {
                    sendJson(ws, {{"type", "error"}, {"reason", "missing room"}});
                    return;
                }

                std::weak_ptr<ClientState> weak = st;
                RtpTransport* tp = st->transport.get();
                const std::string id = st->id;
                const std::string room = st->room;
                auto joinedSfu = std::make_shared<std::atomic<bool>>(false);

                RtpTransport::Callbacks cbs;
                // Candidate: marshal the ws->send onto the loop thread.
                cbs.onLocalCandidate = [weak, loop](const std::string& cand) {
                    loop->defer([weak, cand]() {
                        auto s = weak.lock();
                        if (!s || !s->alive.load()) return;
                        sendJson(s->ws, {{"type", "ice"}, {"payload", cand}});
                    });
                };
                // Connected: register with the SFU on the loop thread (serialized
                // with the close handler, so add never races remove).
                cbs.onStateChanged = [weak, sfu, room, id, tp, joinedSfu, loop](IceState s) {
                    if (s != IceState::Connected && s != IceState::Completed) return;
                    loop->defer([weak, sfu, room, id, tp, joinedSfu]() {
                        auto st2 = weak.lock();
                        if (!st2 || !st2->alive.load()) return;
                        if (joinedSfu->exchange(true)) return;
                        auto sink = [tp](const RtpPacket& p) { (void)tp->send(p); };
                        (void)sfu->addParticipant(room, id, sink);
                        std::cout << "[sfu] participant " << id << " connected to room "
                                  << room << "\n";
                    });
                };
                // Inbound RTP: forward directly (no socket, off the loop thread).
                cbs.onPacket = [sfu, room, id](const RtpPacket& pkt) {
                    sfu->routePacket(room, id, pkt);
                };

                if (!st->transport->initialize(iceCfg, std::move(cbs))) {
                    sendJson(ws, {{"type", "error"}, {"reason", "ICE init failed"}});
                    return;
                }
                (void)st->transport->startGathering();
                psd->state = st;
                // The reused SignalingClient parses "id" as a number.
                sendJson(ws, {{"type", "joined"}, {"room", st->room}, {"id", numId}});
                std::cout << "[sfu] " << st->id << " joined room " << st->room << "\n";
                return;
            }

            if (!psd->state) return;  // must join first
            ClientState& st = *psd->state;

            if (type == "offer") {
                const std::string sdp = j.value("payload", std::string{});
                if (!st.transport->setRemoteDescription(sdp)) {
                    sendJson(ws, {{"type", "error"}, {"reason", "bad offer"}});
                    return;
                }
                auto answer = st.transport->localDescription();
                if (answer) sendJson(ws, {{"type", "answer"}, {"payload", answer.value()}});
                return;
            }
            if (type == "ice") {
                const std::string cand = j.value("payload", std::string{});
                (void)st.transport->addRemoteCandidate(cand);
                return;
            }
            if (type == "leave") {
                ws->end(1000, "left");
                return;
            }
        },
        .close = [sfu](Ws* ws, int, std::string_view) {
            auto* psd = static_cast<PerSocketData*>(ws->getUserData());
            if (!psd->state) return;
            psd->state->alive.store(false);
            (void)sfu->removeParticipant(psd->state->room, psd->state->id);
            std::cout << "[sfu] " << psd->state->id << " left room "
                      << psd->state->room << "\n";
            psd->state.reset();  // destroys the RtpTransport (loop thread)
        },
    });

    app.listen(port, [port](us_listen_socket_t* ls) {
        if (ls) {
            std::cout << "SFU server listening on ws://0.0.0.0:" << port << "\n";
        } else {
            std::cerr << "Failed to listen on port " << port << "\n";
            std::exit(1);
        }
    });
    app.run();
    return 0;
}
