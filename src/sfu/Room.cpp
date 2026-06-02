#include "sfu/Room.h"

#include <limits>
#include <utility>

namespace vc {

Room::Room(std::string id, KeyframeRequestFn keyframeRequest, std::size_t maxParticipants)
    : id_(std::move(id)),
      keyframeRequest_(std::move(keyframeRequest)),
      maxParticipants_(maxParticipants) {}

Result<Participant*, Error> Room::add(const ParticipantId& id, PacketSink sink) {
    std::vector<ParticipantId> existingVideoSenders;
    Participant* added = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (participants_.find(id) != participants_.end()) {
            return fail(Error::InvalidArgument);
        }
        if (participants_.size() >= maxParticipants_) {
            return fail(Error::ResourceExhausted);
        }
        // Collect existing video senders before inserting the newcomer.
        for (const auto& [pid, p] : participants_) {
            if (p->hasVideo()) existingVideoSenders.push_back(pid);
        }
        auto p = std::make_unique<Participant>(id, std::move(sink));
        p->setNeedsKeyframe(true);
        added = p.get();
        participants_.emplace(id, std::move(p));
    }

    // Ask existing senders for a keyframe so the newcomer can decode. Done
    // outside the lock: the callback may re-enter the SFU/encoder layer.
    if (keyframeRequest_) {
        for (const auto& senderId : existingVideoSenders) {
            keyframeRequest_(senderId);
        }
    }
    return added;
}

Status Room::remove(const ParticipantId& id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = participants_.find(id);
    if (it == participants_.end()) return fail(Error::NotFound);
    participants_.erase(it);
    recomputeTargetsLocked();
    return ok();
}

Participant* Room::find(const ParticipantId& id) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = participants_.find(id);
    return it == participants_.end() ? nullptr : it->second.get();
}

void Room::forward(const ParticipantId& senderId, const RtpPacket& pkt) {
    std::lock_guard<std::mutex> lock(mu_);
    if (participants_.find(senderId) == participants_.end()) return; // sender left
    for (auto& [pid, p] : participants_) {
        if (pid == senderId) continue; // never echo to the sender
        p->deliver(pkt);
    }
}

void Room::onRemb(const ParticipantId& id, uint32_t bitrateBps) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = participants_.find(id);
    if (it == participants_.end()) return;
    it->second->setRembEstimate(bitrateBps);
    recomputeTargetsLocked();
}

uint32_t Room::targetBitrate(const ParticipantId& senderId) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = participants_.find(senderId);
    return it == participants_.end() ? 0u : it->second->targetBitrate();
}

std::size_t Room::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return participants_.size();
}

void Room::recomputeTargetsLocked() {
    // Each sender's target is the min REMB estimate over its receivers (all the
    // other participants). Receivers with no estimate (0) are ignored; if no
    // receiver has reported, the target is left at 0 ("unconstrained").
    for (auto& [senderId, sender] : participants_) {
        uint32_t minEstimate = std::numeric_limits<uint32_t>::max();
        bool any = false;
        for (auto& [recvId, recv] : participants_) {
            if (recvId == senderId) continue;
            const uint32_t est = recv->rembEstimate();
            if (est == 0) continue;
            any = true;
            if (est < minEstimate) minEstimate = est;
        }
        sender->setTargetBitrate(any ? minEstimate : 0u);
    }
}

} // namespace vc
