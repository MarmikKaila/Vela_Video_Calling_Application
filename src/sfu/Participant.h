#pragma once

// Participant — per-endpoint routing state inside an SFU Room.
//
// A Participant models one connected endpoint as seen by the Selective
// Forwarding Unit. The SFU never decodes media; it forwards encoded RtpPackets
// verbatim. A Participant therefore holds only *routing-level* state:
//
//   * an opaque ParticipantId (stable handle the app assigns),
//   * the audio and video SSRCs this endpoint *sends* (so a packet can be
//     attributed to a media kind without inspecting the payload),
//   * a downstream send sink — the callback the Room invokes to deliver a
//     forwarded packet *to* this endpoint (i.e. its receive path),
//   * last-forwarded sequence bookkeeping per SSRC (for diagnostics / loss
//     detection on the egress path),
//   * the current target send bitrate, driven by REMB feedback (see SFUServer).
//   * whether this endpoint still needs a keyframe before it can decode.
//
// SSRC remapping: this SFU forwards verbatim and keys downstream streams by the
// *sender's* SSRC. We do NOT rewrite SSRCs. Receivers therefore observe each
// remote sender under that sender's original SSRC, which is the simplest and
// most transparent SFU topology. Any future simulcast/layer selection would add
// remapping here; it is intentionally absent now.
//
// Thread-safety: a Participant is owned by exactly one Room. The Room serializes
// all access to a Participant under its own lock, so Participant itself is not
// internally synchronized. The send sink may be invoked from any transport
// thread the Room forwards on; the sink implementation must be thread-safe.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "network/RtpPacket.h"

namespace vc {

// Opaque, app-assigned identifier for a participant. Stable for the lifetime of
// the participant's membership in a room. Kept as a string so the app can use
// session tokens / peer ids without the SFU imposing a numbering scheme.
using ParticipantId = std::string;

// Sink invoked to deliver one forwarded RtpPacket to this participant's
// receive path (its transport's send-to-this-peer function). Passed by const
// reference; the sink must copy/serialize before returning if it needs to
// retain the data. May be called concurrently from multiple transport threads
// — implementations must be thread-safe.
using PacketSink = std::function<void(const RtpPacket&)>;

// Per-endpoint routing state. Construct with an id and a send sink, then
// register the SSRCs this endpoint will send on.
class Participant {
public:
    // Construct a participant. `id` is the opaque handle; `sink` is the
    // downstream delivery callback (may be empty if the endpoint is send-only,
    // in which case forwarded packets to it are simply dropped).
    Participant(ParticipantId id, PacketSink sink);

    // --- Identity ------------------------------------------------------------

    [[nodiscard]] const ParticipantId& id() const noexcept { return id_; }

    // --- SSRC registration ---------------------------------------------------
    // The app tells the SFU which SSRCs this endpoint sends so inbound packets
    // can be attributed to a media kind. An SSRC of 0 means "not set".

    void setAudioSsrc(uint32_t ssrc) noexcept { audioSsrc_ = ssrc; }
    void setVideoSsrc(uint32_t ssrc) noexcept { videoSsrc_ = ssrc; }

    [[nodiscard]] uint32_t audioSsrc() const noexcept { return audioSsrc_; }
    [[nodiscard]] uint32_t videoSsrc() const noexcept { return videoSsrc_; }

    // True if this endpoint is registered as sending a video stream. Used by
    // the keyframe-on-join logic to find senders that must emit a fresh IDR.
    [[nodiscard]] bool hasVideo() const noexcept { return videoSsrc_ != 0; }

    // --- Egress (deliver TO this participant) --------------------------------

    // Deliver a packet to this endpoint via its sink, recording the last
    // sequence number forwarded for the packet's SSRC. No-op if no sink is set.
    void deliver(const RtpPacket& pkt);

    // Last sequence number forwarded to this endpoint for `ssrc`, if any packet
    // for that SSRC has been delivered.
    [[nodiscard]] std::optional<uint16_t> lastForwardedSeq(uint32_t ssrc) const;

    // --- Keyframe bookkeeping ------------------------------------------------
    // A freshly joined participant cannot decode video mid-GOP; it needs the
    // next IDR. The SFUServer sets this on join and clears it once a keyframe
    // has been forwarded to the endpoint.

    void setNeedsKeyframe(bool needs) noexcept { needsKeyframe_ = needs; }
    [[nodiscard]] bool needsKeyframe() const noexcept { return needsKeyframe_; }

    // --- Bitrate / ABR -------------------------------------------------------
    // The target send bitrate (bits per second) the encoder feeding this
    // endpoint should aim for. Driven by the SFU from REMB feedback: the
    // encoder consumes this via VideoEncoder::setBitrate(). 0 means "unset".

    void setTargetBitrate(uint32_t bps) noexcept { targetBitrateBps_ = bps; }
    [[nodiscard]] uint32_t targetBitrate() const noexcept { return targetBitrateBps_; }

    // The most recent REMB-style receive bitrate estimate reported BY this
    // endpoint (what it believes it can receive). The SFU mins these across a
    // sender's receivers to drive that sender's target. 0 means "no estimate".

    void setRembEstimate(uint32_t bps) noexcept { rembEstimateBps_ = bps; }
    [[nodiscard]] uint32_t rembEstimate() const noexcept { return rembEstimateBps_; }

private:
    ParticipantId id_;
    PacketSink sink_;

    uint32_t audioSsrc_ = 0;
    uint32_t videoSsrc_ = 0;

    bool needsKeyframe_ = false;

    uint32_t targetBitrateBps_ = 0;  // computed by the SFU, consumed by encoder
    uint32_t rembEstimateBps_ = 0;   // reported by this endpoint's receiver

    // Last sequence forwarded to this endpoint, keyed by source SSRC.
    std::unordered_map<uint32_t, uint16_t> lastForwardedSeq_;
};

} // namespace vc
