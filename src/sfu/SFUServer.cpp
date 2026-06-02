#include "sfu/SFUServer.h"

#include <utility>

namespace vc {

Result<Participant*, Error> SFUServer::addParticipant(const std::string& roomId,
                                                      const ParticipantId& participantId,
                                                      PacketSink sink) {
    Room* room = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = rooms_.find(roomId);
        if (it == rooms_.end()) {
            it = rooms_.emplace(roomId,
                                std::make_unique<Room>(roomId, keyframeRequest_)).first;
        }
        room = it->second.get();
    }
    // Room::add runs its keyframe-request callbacks outside the server lock.
    return room->add(participantId, std::move(sink));
}

Status SFUServer::removeParticipant(const std::string& roomId,
                                    const ParticipantId& participantId) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = rooms_.find(roomId);
    if (it == rooms_.end()) return fail(Error::NotFound);
    Status s = it->second->remove(participantId);
    if (it->second->size() == 0) rooms_.erase(it); // reclaim empty room
    return s;
}

void SFUServer::routePacket(const std::string& roomId, const ParticipantId& senderId,
                            const RtpPacket& pkt) {
    Room* room = findRoom(roomId);
    if (room) room->forward(senderId, pkt);
}

void SFUServer::onRemb(const std::string& roomId, const ParticipantId& participantId,
                       uint32_t bitrateBps) {
    if (Room* room = findRoom(roomId)) room->onRemb(participantId, bitrateBps);
}

uint32_t SFUServer::targetBitrate(const std::string& roomId,
                                  const ParticipantId& senderId) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = rooms_.find(roomId);
    return it == rooms_.end() ? 0u : it->second->targetBitrate(senderId);
}

std::size_t SFUServer::roomCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return rooms_.size();
}

Room* SFUServer::findRoom(const std::string& roomId) {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = rooms_.find(roomId);
    return it == rooms_.end() ? nullptr : it->second.get();
}

} // namespace vc
