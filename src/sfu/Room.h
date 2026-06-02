#pragma once

// Room — a single conference session inside the SFU.
//
// A Room owns its Participants and implements the core Selective Forwarding
// rule: an encoded RtpPacket received from one participant is forwarded
// verbatim to every OTHER participant, and never echoed back to the sender. The
// SFU does not decode, transcode, or mix — it routes encoded RTP.
//
// Two control behaviours live here because they are per-room policy:
//   * Keyframe-on-join: a newly added participant cannot decode an in-progress
//     video stream until the next IDR, so on join the Room asks each existing
//     video sender (via the injected KeyframeRequestFn) to emit a keyframe.
//   * Bitrate adaptation: each participant reports a REMB receive-bitrate
//     estimate; a sender's target bitrate is the minimum estimate across its
//     receivers, so the slowest receiver bounds the encode bitrate.
//
// Thread-safety: every public method locks an internal mutex, so the Room may
// be driven from multiple transport threads. Participant objects are only
// touched while that lock is held.

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/Error.h"
#include "network/RtpPacket.h"
#include "sfu/Participant.h"

namespace vc {

// Called when an existing video sender should produce a fresh keyframe (PLI
// semantics). The app wires this to that sender's VideoEncoder::forceKeyframe().
using KeyframeRequestFn = std::function<void(const ParticipantId& sender)>;

class Room {
public:
    static constexpr std::size_t kMaxParticipants = 8;

    explicit Room(std::string id, KeyframeRequestFn keyframeRequest = {},
                  std::size_t maxParticipants = kMaxParticipants);

    [[nodiscard]] const std::string& id() const noexcept { return id_; }

    // Add a participant with the given egress sink. Fails with ResourceExhausted
    // when the room is full, or InvalidArgument if the id already exists. On
    // success the new participant is flagged needsKeyframe and each existing
    // video sender is asked (via KeyframeRequestFn) to emit an IDR.
    [[nodiscard]] Result<Participant*, Error> add(const ParticipantId& id, PacketSink sink);

    [[nodiscard]] Status remove(const ParticipantId& id);

    [[nodiscard]] Participant* find(const ParticipantId& id);

    // Forward one packet from `senderId` to every other participant. No-op (not
    // an error) if the sender is unknown — late packets after a leave are benign.
    void forward(const ParticipantId& senderId, const RtpPacket& pkt);

    // Record a participant's REMB receive estimate (bits/s) and recompute the
    // target send bitrate of every sender it receives.
    void onRemb(const ParticipantId& id, uint32_t bitrateBps);

    // Target send bitrate (bits/s) for `senderId`: the minimum REMB estimate
    // across the participants that receive it (everyone else). 0 if no receiver
    // has reported an estimate yet.
    [[nodiscard]] uint32_t targetBitrate(const ParticipantId& senderId) const;

    [[nodiscard]] std::size_t size() const;

private:
    // Recompute and store every sender's target bitrate. Caller holds mu_.
    void recomputeTargetsLocked();

    std::string id_;
    KeyframeRequestFn keyframeRequest_;
    std::size_t maxParticipants_;

    mutable std::mutex mu_;
    std::unordered_map<ParticipantId, std::unique_ptr<Participant>> participants_;
};

} // namespace vc
