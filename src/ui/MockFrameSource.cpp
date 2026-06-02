#include "ui/MockFrameSource.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/Clock.h"

namespace vc::ui {

MockFrameSource::MockFrameSource(int width, int height, int fps, QObject* parent)
    : QObject(parent),
      width_(std::max(2, width)),
      height_(std::max(2, height)),
      fps_(std::max(1, fps)) {
    // A handful of buffers lets a couple of frames be in flight (held by a
    // widget while the next is produced) without stalling.
    pool_ = FramePool::create(VideoFrame::i420Size(width_, height_), 4);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &MockFrameSource::tick);
}

Status MockFrameSource::start() {
    if (!pool_ || width_ <= 0 || height_ <= 0) {
        return fail(Error::InvalidArgument);
    }
    if (timer_.isActive()) {
        return ok();
    }
    timer_.start(1000 / fps_);
    return ok();
}

void MockFrameSource::stop() {
    timer_.stop();
}

void MockFrameSource::tick() {
    auto r = generateOne();
    if (!r) {
        return; // pool exhausted -> drop this tick (back-pressure)
    }
    emit frameReady(r.value());
}

Result<VideoFrame, Error> MockFrameSource::generateOne() {
    auto buf = pool_->acquire();
    if (!buf) {
        return fail(Error::ResourceExhausted);
    }
    VideoFrame frame =
        VideoFrame::makeI420(std::move(buf), width_, height_, MediaClock::now());
    if (!frame.valid()) {
        return fail(Error::Internal);
    }
    fillPattern(frame, frameCounter_++);
    return frame;
}

void MockFrameSource::fillPattern(VideoFrame& frame, uint64_t phase) const {
    const int w = frame.width;
    const int h = frame.height;
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;

    // Horizontal scroll offset so the bars animate over time.
    const int scroll = static_cast<int>(phase * 2);

    // Eight classic color bars (RGB), converted to BT.601 limited-range YUV.
    struct Yuv { uint8_t y, u, v; };
    static const Yuv kBars[8] = {
        {235, 128, 128}, // white
        {210,  16, 146}, // yellow
        {170, 166,  16}, // cyan
        {145,  54,  34}, // green
        {106, 202, 222}, // magenta
        { 81,  90, 240}, // red
        { 41, 240, 110}, // blue
        { 16, 128, 128}, // black
    };

    // --- Y plane: scrolling color bars with a subtle vertical gradient. ---
    Plane yp = frame.planes[0];
    for (int row = 0; row < h; ++row) {
        uint8_t* dst = yp.data + static_cast<std::size_t>(row) * yp.stride;
        const int grad = (row * 24) / std::max(1, h); // 0..24 luma ramp
        for (int col = 0; col < w; ++col) {
            const int barIdx = (((col + scroll) * 8) / std::max(1, w)) & 7;
            int yv = kBars[barIdx].y + grad - 12;
            dst[col] = static_cast<uint8_t>(std::clamp(yv, 16, 235));
        }
    }

    // --- U / V planes (half resolution). ---
    Plane up = frame.planes[1];
    Plane vp = frame.planes[2];
    for (int row = 0; row < ch; ++row) {
        uint8_t* uDst = up.data + static_cast<std::size_t>(row) * up.stride;
        uint8_t* vDst = vp.data + static_cast<std::size_t>(row) * vp.stride;
        for (int col = 0; col < cw; ++col) {
            const int fullCol = col * 2;
            const int barIdx = (((fullCol + scroll) * 8) / std::max(1, w)) & 7;
            uDst[col] = kBars[barIdx].u;
            vDst[col] = kBars[barIdx].v;
        }
    }
}

} // namespace vc::ui
