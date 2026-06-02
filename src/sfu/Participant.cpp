#include "sfu/Participant.h"

#include <utility>

namespace vc {

Participant::Participant(ParticipantId id, PacketSink sink)
    : id_(std::move(id)), sink_(std::move(sink)) {}

void Participant::deliver(const RtpPacket& pkt) {
    // Record egress bookkeeping even if there is no sink, so loss/ordering can
    // still be reasoned about; but only invoke the sink when one is present.
    lastForwardedSeq_[pkt.ssrc] = pkt.sequenceNumber;
    if (sink_) {
        sink_(pkt);
    }
}

std::optional<uint16_t> Participant::lastForwardedSeq(uint32_t ssrc) const {
    const auto it = lastForwardedSeq_.find(ssrc);
    if (it == lastForwardedSeq_.end()) {
        return std::nullopt;
    }
    return it->second;
}

} // namespace vc
