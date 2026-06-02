#pragma once

// SignalingClient — client side of the signaling protocol.
//
// Connects to the standalone signaling server (see signaling-server/) over a
// WebSocket, joins a room, and exchanges SDP offers/answers and ICE candidates
// with the other peers in that room. Incoming events are surfaced through
// callbacks.
//
// TRANSPORT
// ---------
// The public API is deliberately transport-agnostic: nothing in this header
// mentions WebSockets, sockets, or any third-party type. The bundled
// implementation (SignalingClient.cpp) uses a small, self-contained RFC 6455
// WebSocket client running on its own background thread (uWebSockets ships a
// usable *server* but only a stub client, so we do not depend on it). The
// implementation could be swapped for any other transport without touching
// callers.
//
// THREADING
// ---------
// connect()/disconnect() are synchronous from the caller's thread. All
// callbacks are invoked from the internal network thread; handlers must not
// block it for long and must do their own synchronisation. The send* methods
// are thread-safe and may be called from any thread.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "common/Error.h"

namespace vc::signaling {

// Server-assigned identifier of a peer within a room. Matches the server's
// ParticipantId. 0 means "broadcast to the whole room" when used as a target.
using PeerId = std::uint64_t;

// Configuration for a SignalingClient connection.
struct SignalingConfig {
    std::string host = "127.0.0.1";  // server host / IP
    std::uint16_t port = 8080;        // server port
    std::string path = "/";           // WebSocket request path
    std::string room;                  // room to join on connect (required)
};

// Callbacks delivered on the network thread. Any may be left unset.
struct SignalingCallbacks {
    // Local join acknowledged: `self` is this client's PeerId.
    std::function<void(PeerId self)> onJoined;
    // A new peer entered the room.
    std::function<void(PeerId peer)> onPeerJoined;
    // A peer left the room.
    std::function<void(PeerId peer)> onPeerLeft;
    // SDP offer received from `from`; `sdp` is the payload string.
    std::function<void(PeerId from, const std::string& sdp)> onOffer;
    // SDP answer received from `from`.
    std::function<void(PeerId from, const std::string& sdp)> onAnswer;
    // ICE candidate received from `from`; `candidate` is the SDP candidate
    // line as gathered by the remote NATTraversal.
    std::function<void(PeerId from, const std::string& candidate)> onIceCandidate;
    // Transport-level error (connection dropped, parse failure, etc.).
    std::function<void(Error err, const std::string& detail)> onError;
};

// WebSocket signaling client. One instance manages one connection/room.
class SignalingClient {
public:
    SignalingClient();
    ~SignalingClient();

    SignalingClient(const SignalingClient&) = delete;
    SignalingClient& operator=(const SignalingClient&) = delete;

    // Install callbacks. Must be called before connect() (they are read from
    // the network thread). Replaces any previously-set callbacks.
    void setCallbacks(SignalingCallbacks cbs);

    // Open the connection, perform the WebSocket handshake, and send a "join"
    // for cfg.room. Blocks until the TCP+WS handshake completes or fails.
    // Fails with:
    //   InvalidArgument — empty room
    //   AlreadyRunning  — already connected
    //   NetworkError    — DNS/connect/handshake failure
    [[nodiscard]] Status connect(const SignalingConfig& cfg);

    // Send a "leave", close the socket, and join the network thread. Safe to
    // call repeatedly and from the destructor.
    void disconnect();

    // True once connect() has succeeded and the link is still up.
    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }

    // The PeerId assigned by the server, or 0 before "joined" arrives.
    [[nodiscard]] PeerId selfId() const noexcept { return selfId_.load(); }

    // Send an SDP offer to `to` (0 = broadcast to the room).
    Status sendOffer(PeerId to, std::string_view sdp);
    // Send an SDP answer to `to`.
    Status sendAnswer(PeerId to, std::string_view sdp);
    // Send a single ICE candidate to `to`.
    Status sendIceCandidate(PeerId to, std::string_view candidate);

private:
    // Frame a signaling message of `type` with string payload and queue it for
    // transmission. Thread-safe.
    Status sendMessage(std::string_view type, PeerId to, std::string_view payload);

    // Background receive loop: reads WebSocket frames, parses JSON, dispatches
    // callbacks. Runs until the socket closes or disconnect() is requested.
    void runRecvLoop();

    // Low-level helpers (implemented in the .cpp).
    Status doHandshake();
    Status sendTextFrame(std::string_view text);  // masked client frame
    void closeSocket() noexcept;

    SignalingCallbacks cbs_;
    SignalingConfig cfg_;

    int fd_ = -1;                       // TCP socket, -1 when closed
    std::thread recvThread_;
    std::mutex sendMutex_;              // serialises socket writes
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<PeerId> selfId_{0};
};

}  // namespace vc::signaling
