#pragma once

// NATTraversal — thin RAII wrapper around libjuice's ICE agent.
//
// Performs ICE connectivity establishment for one peer-to-peer media flow:
//   * creates an ICE agent configured with a STUN server (default
//     stun.l.google.com:19302) so it can discover server-reflexive
//     candidates, while ALWAYS also gathering host candidates so the agent
//     works on a LAN / fully offline;
//   * exposes the local ICE description (ufrag/pwd + candidates) to hand to the
//     SignalingClient;
//   * surfaces each gathered candidate through a callback so the caller can
//     relay it (trickle ICE) over the signaling channel;
//   * accepts the remote description and remote candidates received from the
//     peer.
//
// One NATTraversal instance corresponds to one juice_agent_t and therefore one
// negotiated connection. Create one per remote peer.
//
// THREADING
// ---------
// libjuice invokes its callbacks from an internal thread. The std::function
// callbacks installed here are therefore called off the caller's thread; they
// must be thread-safe. The public methods are safe to call from the caller's
// thread while gathering is in progress (libjuice guards its own agent state).

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "common/Error.h"

struct juice_agent;  // opaque libjuice type, forward-declared

namespace vc::signaling {

// Coarse mirror of juice_state_t so callers don't need the libjuice header.
enum class IceState {
    Disconnected,
    Gathering,
    Connecting,
    Connected,   // a usable candidate pair was found
    Completed,   // ICE checks finished
    Failed,
};

[[nodiscard]] std::string_view to_string(IceState s) noexcept;

// Configuration for an ICE agent.
struct NATConfig {
    // STUN server for server-reflexive candidates. Empty disables STUN and
    // gathers host candidates only (useful for strictly-local/offline tests).
    std::string stunHost = "stun.l.google.com";
    std::uint16_t stunPort = 19302;
};

// Callbacks from libjuice's internal thread. Any may be left unset.
struct NATCallbacks {
    // A locally gathered candidate (SDP "a=candidate:..." line). Relay this to
    // the remote peer via signaling as it arrives (trickle ICE).
    std::function<void(const std::string& candidate)> onLocalCandidate;
    // Candidate gathering finished; localDescription() is now complete.
    std::function<void()> onGatheringDone;
    // Connection state changed.
    std::function<void(IceState state)> onStateChanged;
    // Application data received over the established ICE connection.
    std::function<void(const std::uint8_t* data, std::size_t len)> onData;
};

// RAII owner of a single libjuice ICE agent.
class NATTraversal {
public:
    NATTraversal();
    ~NATTraversal();

    NATTraversal(const NATTraversal&) = delete;
    NATTraversal& operator=(const NATTraversal&) = delete;

    // Create the underlying agent with `cfg`. Install callbacks before calling
    // so no early candidate is missed. Fails AlreadyRunning if already
    // initialised, NetworkError if libjuice could not create the agent.
    [[nodiscard]] Status initialize(const NATConfig& cfg, NATCallbacks cbs);

    // Begin gathering candidates (host + STUN). Candidates arrive via
    // onLocalCandidate; completion via onGatheringDone. Fails NotInitialized
    // if initialize() has not succeeded.
    [[nodiscard]] Status startGathering();

    // Snapshot of the local ICE description (ufrag/pwd + any candidates
    // gathered so far). Send this to the peer as the SDP offer/answer payload.
    [[nodiscard]] Result<std::string, Error> localDescription() const;

    // Apply the remote peer's ICE description (their ufrag/pwd line block).
    [[nodiscard]] Status setRemoteDescription(std::string_view sdp);

    // Add one remote ICE candidate (a single "a=candidate:..." line) received
    // via signaling.
    [[nodiscard]] Status addRemoteCandidate(std::string_view candidate);

    // Signal that the remote peer has finished gathering (end-of-candidates).
    [[nodiscard]] Status setRemoteGatheringDone();

    // Send application data once the connection is established. Fails
    // NetworkError if the agent is not connected.
    [[nodiscard]] Status send(const std::uint8_t* data, std::size_t len);

    // Current connection state.
    [[nodiscard]] IceState state() const noexcept { return state_.load(); }

    // Invoked by the libjuice trampolines (defined in the .cpp) to deliver
    // events. Not part of the public API; public only so the file-local
    // trampolines, which must match libjuice's exact C signatures, can reach
    // them. `juiceState` is the raw juice_state_t value.
    void handleStateChanged(int juiceState);
    void handleCandidate(const char* sdp);
    void handleGatheringDone();
    void handleRecv(const char* data, std::size_t size);

private:
    juice_agent* agent_ = nullptr;
    NATCallbacks cbs_;
    NATConfig cfg_;
    // libjuice copies the stun host string only by pointer in some builds; keep
    // the backing storage alive for the agent's lifetime.
    std::string stunHostStorage_;
    std::atomic<IceState> state_{IceState::Disconnected};
};

}  // namespace vc::signaling
