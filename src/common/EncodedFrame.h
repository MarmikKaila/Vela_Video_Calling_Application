#pragma once

// EncodedFrame — compressed media leaving an encoder, entering the RTP
// packetizer, and forwarded verbatim by the SFU (which must never decode).
//
// For H.264 this holds one access unit (the NAL units for a single picture) in
// Annex-B or AVCC form; for Opus it holds one encoded audio packet. The bytes
// are reference-counted (shared_ptr) so the SFU can fan one encoded frame out
// to N participants without copying the payload.

#include <cstdint>
#include <memory>
#include <vector>

#include "common/Clock.h"
#include "common/Types.h"

namespace vc {

class EncodedFrame {
public:
    MediaKind kind = MediaKind::Video;
    VideoCodec videoCodec = VideoCodec::Unknown; // valid when kind == Video
    AudioCodec audioCodec = AudioCodec::Unknown; // valid when kind == Audio

    // True for a video keyframe (IDR). The SFU/encoder uses this to satisfy
    // keyframe requests when a new participant joins, and the jitter buffer uses
    // it as a resync point after loss.
    bool keyframe = false;

    uint32_t rtpTimestamp = 0; // codec clock (90 kHz video / 48 kHz audio)
    SteadyTime captureTime{};  // original capture instant, for latency tracking

    [[nodiscard]] const uint8_t* data() const noexcept {
        return payload_ ? payload_->data() : nullptr;
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return payload_ ? payload_->size() : 0;
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    // Take ownership of an encoded byte buffer (the encoder produces it once,
    // then it is read-only and shareable).
    void setPayload(std::vector<uint8_t> bytes) {
        payload_ = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    }
    [[nodiscard]] const std::shared_ptr<const std::vector<uint8_t>>& payload() const noexcept {
        return payload_;
    }

private:
    std::shared_ptr<const std::vector<uint8_t>> payload_;
};

} // namespace vc
