#pragma once

// SFUServer — the multi-room Selective Forwarding Unit facade.
//
// Owns the set of Rooms and provides the entry points a transport layer calls:
// participants join/leave rooms, inbound RTP is routed into the right room, and
// REMB feedback drives per-sender target bitrates. Rooms are created on first
// join and destroyed when their last participant leaves.
//
// The SFU is transport-agnostic: it never touches sockets. The app supplies a
// PacketSink per participant (how to deliver bytes to that endpoint) and a
// single KeyframeRequestFn (how to ask a sender's encoder for an IDR).
//
// Thread-safety: the room registry is mutex-protected, and each Room is
// internally synchronized, so the server may be driven from many threads.

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/Error.h"
#include "network/RtpPacket.h"
#include "sfu/Participant.h"
#include "sfu/Room.h"

namespace vc {

class SFUServer {
public:
    SFUServer() = default;

    // Set the callback used for keyframe-on-join across all rooms. Wire this to
    // the encoder feeding the named sender (sender->forceKeyframe()).
    void setKeyframeRequestCallback(KeyframeRequestFn fn) { keyframeRequest_ = std::move(fn); }

    // Add `participantId` to `roomId` (creating the room if needed) with the
    // given egress sink. Propagates Room::add errors (ResourceExhausted when the
    // room is full, InvalidArgument on duplicate id).
    [[nodiscard]] Result<Participant*, Error> addParticipant(const std::string& roomId,
                                                             const ParticipantId& participantId,
                                                             PacketSink sink);

    // Remove a participant; destroys the room when it becomes empty.
    [[nodiscard]] Status removeParticipant(const std::string& roomId,
                                           const ParticipantId& participantId);

    // Route one inbound packet from `senderId` in `roomId` to that room's other
    // participants. No-op if the room is unknown.
    void routePacket(const std::string& roomId, const ParticipantId& senderId,
                     const RtpPacket& pkt);

    // Feed a participant's REMB receive-bitrate estimate (bits/s).
    void onRemb(const std::string& roomId, const ParticipantId& participantId,
                uint32_t bitrateBps);

    // Target send bitrate (bits/s) for a sender; 0 if unknown/unconstrained.
    [[nodiscard]] uint32_t targetBitrate(const std::string& roomId,
                                         const ParticipantId& senderId) const;

    [[nodiscard]] std::size_t roomCount() const;
    [[nodiscard]] Room* findRoom(const std::string& roomId);

private:
    KeyframeRequestFn keyframeRequest_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::unique_ptr<Room>> rooms_;
};

} // namespace vc
