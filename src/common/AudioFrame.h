#pragma once

// AudioFrame — a block of interleaved 16-bit PCM samples.
//
// Opus encodes fixed-duration frames (typically 20 ms). At 48 kHz mono that is
// 960 samples; stereo doubles the sample count. Like VideoFrame, the PCM data
// is backed by a pooled buffer so the audio path allocates nothing steady-state.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/Clock.h"
#include "common/FramePool.h"
#include "common/Types.h"

namespace vc {

class AudioFrame {
public:
    AudioFrame() = default;

    AudioFormat format{};
    int16_t* samples = nullptr; // interleaved L,R,L,R... ; length = frames*channels
    int sampleCount = 0;        // samples PER CHANNEL (i.e. frame count)

    SteadyTime captureTime{};
    uint32_t rtpTimestamp = 0;  // 48 kHz clock

    [[nodiscard]] bool valid() const noexcept {
        return samples != nullptr && sampleCount > 0 && format.channels > 0;
    }

    // Duration of this block, derived from sampleCount and the sample rate.
    [[nodiscard]] std::chrono::nanoseconds duration() const noexcept {
        if (format.sampleRateHz <= 0) return std::chrono::nanoseconds{0};
        return std::chrono::nanoseconds{
            static_cast<int64_t>(sampleCount) * 1'000'000'000 / format.sampleRateHz};
    }

    [[nodiscard]] static AudioFrame makePcm(std::shared_ptr<FramePool::Buffer> buf,
                                            AudioFormat fmt, int samplesPerChannel,
                                            SteadyTime captured) {
        AudioFrame a;
        const std::size_t bytes =
            static_cast<std::size_t>(samplesPerChannel) * fmt.channels * sizeof(int16_t);
        if (!buf || samplesPerChannel <= 0 || fmt.channels <= 0 || buf->capacity() < bytes)
            return a;
        a.format = fmt;
        a.samples = reinterpret_cast<int16_t*>(buf->data());
        a.sampleCount = samplesPerChannel;
        a.captureTime = captured;
        a.buffer_ = std::move(buf);
        return a;
    }

    [[nodiscard]] const std::shared_ptr<FramePool::Buffer>& buffer() const noexcept {
        return buffer_;
    }

private:
    std::shared_ptr<FramePool::Buffer> buffer_;
};

} // namespace vc
