#include "signaling/RtpTransport.h"

#include <utility>

namespace vc::signaling {

Status RtpTransport::initialize(const NATConfig& cfg, Callbacks cbs) {
    cbs_ = std::move(cbs);

    NATCallbacks nat;
    nat.onLocalCandidate = [this](const std::string& cand) {
        if (cbs_.onLocalCandidate) cbs_.onLocalCandidate(cand);
    };
    nat.onGatheringDone = [this]() {
        if (cbs_.onGatheringDone) cbs_.onGatheringDone();
    };
    nat.onStateChanged = [this](IceState s) {
        if (cbs_.onStateChanged) cbs_.onStateChanged(s);
    };
    nat.onData = [this](const std::uint8_t* data, std::size_t len) {
        if (!cbs_.onPacket) return;
        // libjuice delivers only application data here, so this should always be
        // an RTP datagram. Parse defensively and drop anything malformed rather
        // than propagate a partial packet into the pipeline.
        auto pkt = RtpPacket::parse(data, len);
        if (pkt) cbs_.onPacket(pkt.value());
    };

    return nat_.initialize(cfg, std::move(nat));
}

Status RtpTransport::send(const RtpPacket& pkt) {
    const std::vector<std::uint8_t> bytes = pkt.serialize();
    return nat_.send(bytes.data(), bytes.size());
}

}  // namespace vc::signaling
