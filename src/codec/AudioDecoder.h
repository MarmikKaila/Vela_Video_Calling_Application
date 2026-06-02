#pragma once

// AudioDecoder — Opus -> interleaved 16-bit PCM AudioFrame, wrapping libopus.
//
// Consumes EncodedFrames carrying Opus packets and produces PCM AudioFrames at
// 48 kHz. Decoded samples are written into a buffer drawn from a FramePool the
// decoder owns, so steady-state playout allocates nothing. rtpTimestamp and
// captureTime are carried through from the input EncodedFrame.
//
// Lifetime: open(format) once before decode(). The OpusDecoder is owned via a
// unique_ptr with a custom deleter (RAII). Not thread-safe.

#include <cstdint>
#include <memory>

#include "common/AudioFrame.h"
#include "common/EncodedFrame.h"
#include "common/Error.h"
#include "common/FramePool.h"
#include "common/Result.h"
#include "common/Types.h"

struct OpusDecoder;

namespace vc {

class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();

    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;
    AudioDecoder(AudioDecoder&&) noexcept = default;
    AudioDecoder& operator=(AudioDecoder&&) noexcept = default;

    // Create the Opus decoder for the given PCM output format (48 kHz, 1 or 2
    // channels). Safe to re-call (resets).
    [[nodiscard]] Status open(AudioFormat format);

    // Decode one Opus packet into a PCM AudioFrame. Returns CodecError on a
    // decode failure, ResourceExhausted if the pool is momentarily empty.
    [[nodiscard]] Result<AudioFrame, Error> decode(const EncodedFrame& frame);

    [[nodiscard]] bool isOpen() const noexcept { return dec_ != nullptr; }

private:
    struct OpusDecoderDeleter {
        void operator()(OpusDecoder* d) const noexcept;
    };

    std::unique_ptr<OpusDecoder, OpusDecoderDeleter> dec_;
    AudioFormat format_{};
    std::shared_ptr<FramePool> pool_;
};

} // namespace vc
