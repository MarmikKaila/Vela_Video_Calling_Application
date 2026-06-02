#pragma once

// VideoFrame — a single decoded/raw video frame flowing through the pipeline.
//
// The frame does NOT own its pixel memory directly; it holds a shared_ptr to a
// FramePool::Buffer that keeps the backing storage alive. Multiple consumers
// (encoder, local preview renderer, the SFU forwarding path) can therefore
// share one frame with zero copies — the buffer recycles when the last
// VideoFrame referencing it is destroyed.
//
// `planes` point INTO that buffer. For I420 the three planes are laid out
// contiguously (Y, then U, then V); helpers below compute the standard layout.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "common/Clock.h"
#include "common/FramePool.h"
#include "common/Types.h"

namespace vc {

struct Plane {
    uint8_t* data = nullptr;
    int stride = 0; // bytes per row (>= width * bytesPerPixel for that plane)
};

class VideoFrame {
public:
    VideoFrame() = default;

    PixelFormat format = PixelFormat::Unknown;
    int width = 0;
    int height = 0;
    std::array<Plane, 3> planes{};

    // Monotonic instant the frame was captured (latency math, playout timing).
    SteadyTime captureTime{};
    // 90 kHz RTP timestamp assigned at capture; preserved end-to-end so the far
    // side and AVSync see a consistent timeline. 0 until assigned.
    uint32_t rtpTimestamp = 0;

    [[nodiscard]] bool valid() const noexcept {
        return format != PixelFormat::Unknown && width > 0 && height > 0 && planes[0].data;
    }

    // Build an I420 frame over a freshly acquired pool buffer, wiring the three
    // plane pointers/strides to the standard tightly-packed layout. Returns an
    // invalid frame if the buffer is too small or null (caller drops it).
    [[nodiscard]] static VideoFrame makeI420(std::shared_ptr<FramePool::Buffer> buf,
                                             int w, int h, SteadyTime captured) {
        VideoFrame f;
        if (!buf || w <= 0 || h <= 0) return f;
        const std::size_t ySize = static_cast<std::size_t>(w) * h;
        const int cw = (w + 1) / 2;
        const int ch = (h + 1) / 2;
        const std::size_t cSize = static_cast<std::size_t>(cw) * ch;
        if (buf->capacity() < ySize + 2 * cSize) return f;

        uint8_t* base = buf->data();
        f.format = PixelFormat::I420;
        f.width = w;
        f.height = h;
        f.planes[0] = {base, w};
        f.planes[1] = {base + ySize, cw};
        f.planes[2] = {base + ySize + cSize, cw};
        f.captureTime = captured;
        f.buffer_ = std::move(buf);
        return f;
    }

    // Minimum I420 buffer size for a w*h frame; size your FramePool with this.
    [[nodiscard]] static std::size_t i420Size(int w, int h) noexcept {
        const std::size_t y = static_cast<std::size_t>(w) * h;
        const std::size_t c = static_cast<std::size_t>((w + 1) / 2) * ((h + 1) / 2);
        return y + 2 * c;
    }

    [[nodiscard]] const std::shared_ptr<FramePool::Buffer>& buffer() const noexcept {
        return buffer_;
    }

private:
    std::shared_ptr<FramePool::Buffer> buffer_; // keeps plane memory alive
};

} // namespace vc
