#pragma once

// AudioEncoder — Opus encoder wrapping libopus.
//
// Consumes interleaved 16-bit PCM AudioFrames at 48 kHz and produces one Opus
// packet per frame in the EncodedFrame payload. Opus operates on fixed
// frame durations; this encoder expects 20 ms frames (960 samples/channel at
// 48 kHz), the conferencing standard. The encoder is configured for low-delay
// voice/audio (OPUS_APPLICATION_AUDIO) at a caller-chosen target bitrate.
//
// rtpTimestamp and captureTime are carried through from the input AudioFrame.
//
// Lifetime: open(format, bitrate) once before encode(). The OpusEncoder is
// owned via a unique_ptr with a custom deleter (RAII). Not thread-safe.

#include <cstdint>
#include <memory>
#include <vector>

#include "common/AudioFrame.h"
#include "common/EncodedFrame.h"
#include "common/Error.h"
#include "common/Result.h"
#include "common/Types.h"

struct OpusEncoder;

namespace vc {

class AudioEncoder {
public:
    AudioEncoder();
    ~AudioEncoder();

    AudioEncoder(const AudioEncoder&) = delete;
    AudioEncoder& operator=(const AudioEncoder&) = delete;
    AudioEncoder(AudioEncoder&&) noexcept = default;
    AudioEncoder& operator=(AudioEncoder&&) noexcept = default;

    // Create the Opus encoder for the given PCM format (must be 48 kHz, 1 or 2
    // channels) at the target bitrate in bits/second. Safe to re-call (resets).
    [[nodiscard]] Status open(AudioFormat format, int bitrateBps);

    // Encode one PCM frame (must be a valid Opus frame size at 48 kHz, e.g. 960
    // samples/channel for 20 ms) into a single Opus packet. Returns
    // InvalidArgument on a format/size mismatch, CodecError on encoder failure,
    // NotInitialized before open().
    [[nodiscard]] Result<EncodedFrame, Error> encode(const AudioFrame& frame);

    [[nodiscard]] bool isOpen() const noexcept { return enc_ != nullptr; }

private:
    struct OpusEncoderDeleter {
        void operator()(OpusEncoder* e) const noexcept;
    };

    std::unique_ptr<OpusEncoder, OpusEncoderDeleter> enc_;
    AudioFormat format_{};
    std::vector<uint8_t> scratch_; // reused output buffer in the hot path
};

} // namespace vc
