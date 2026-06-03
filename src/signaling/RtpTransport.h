#pragma once

// RtpTransport — the bridge between the ICE channel and the RTP packet model.
//
// NATTraversal carries opaque bytes over an established ICE/UDP connection;
// RtpPacket knows how to (de)serialize the RFC 3550 wire format. RtpTransport
// glues the two so the media pipeline can speak in RtpPackets and never touch
// raw datagrams:
//
//   send(RtpPacket)  -> RtpPacket::serialize() -> NATTraversal::send()
//   NATTraversal onData(bytes) -> RtpPacket::parse() -> onPacket(RtpPacket)
//
// All ICE setup (description/candidate exchange, gathering) is forwarded
// straight through to the wrapped NATTraversal so the signaling layer drives it
// exactly as before. One RtpTransport == one ICE connection == one peer link
// (for the SFU server, one per connected client).
//
// THREADING: like NATTraversal, the callbacks fire on libjuice's internal
// thread, so onPacket/onLocalCandidate/onStateChanged must be thread-safe.
// send() may be called from any thread (the capture/encode thread).

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "common/Error.h"
#include "network/RtpPacket.h"
#include "signaling/NATTraversal.h"

namespace vc::signaling {

class RtpTransport {
public:
    struct Callbacks {
        // A fully parsed inbound RTP packet (malformed datagrams are dropped).
        std::function<void(const RtpPacket&)> onPacket;
        // A locally gathered ICE candidate to relay to the peer (trickle ICE).
        std::function<void(const std::string& candidate)> onLocalCandidate;
        // ICE connection state changed (watch for Connected).
        std::function<void(IceState state)> onStateChanged;
        // Local candidate gathering finished; localDescription() is complete.
        std::function<void()> onGatheringDone;
    };

    RtpTransport() = default;

    RtpTransport(const RtpTransport&) = delete;
    RtpTransport& operator=(const RtpTransport&) = delete;

    // Create the ICE agent and install callbacks. `cfg.stunHost` may be left
    // empty for strictly-LAN use (host candidates only). Fails as NATTraversal.
    [[nodiscard]] Status initialize(const NATConfig& cfg, Callbacks cbs);

    // --- ICE setup, forwarded to the wrapped NATTraversal --------------------
    [[nodiscard]] Status startGathering() { return nat_.startGathering(); }
    [[nodiscard]] Result<std::string, Error> localDescription() const {
        return nat_.localDescription();
    }
    [[nodiscard]] Status setRemoteDescription(std::string_view sdp) {
        return nat_.setRemoteDescription(sdp);
    }
    [[nodiscard]] Status addRemoteCandidate(std::string_view candidate) {
        return nat_.addRemoteCandidate(candidate);
    }
    [[nodiscard]] Status setRemoteGatheringDone() {
        return nat_.setRemoteGatheringDone();
    }

    // Serialize and send one RTP packet over the ICE connection. Fails
    // NetworkError if the agent is not yet connected.
    [[nodiscard]] Status send(const RtpPacket& pkt);

    [[nodiscard]] IceState state() const noexcept { return nat_.state(); }

private:
    NATTraversal nat_;
    Callbacks cbs_;
};

}  // namespace vc::signaling
