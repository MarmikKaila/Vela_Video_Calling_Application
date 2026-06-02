#pragma once

// MockFrameSource — a self-contained generator of animated I420 test-pattern
// frames, used to drive VideoWidget without depending on the (separately owned)
// capture module.
//
// It owns a FramePool sized for one I420 frame of the configured resolution and
// produces a moving color-bar / gradient pattern on a QTimer. Each tick it
// acquires a pooled buffer, fills the Y/U/V planes, and emits frameReady(frame).
// If the pool is momentarily exhausted the tick is skipped (back-pressure), the
// same drop-don't-block contract the real pipeline uses.
//
// All members are accessed on the thread that owns the QTimer (the GUI thread
// in the demo), so no internal locking is required here; VideoWidget::setFrame
// handles cross-thread delivery if a caller chooses to move generation off the
// GUI thread.

#include <QObject>
#include <QTimer>
#include <cstdint>
#include <memory>

#include "common/Error.h"
#include "common/FramePool.h"
#include "common/Result.h"
#include "common/VideoFrame.h"

namespace vc::ui {

/// Emits animated I420 test frames on a timer for standalone UI rendering.
class MockFrameSource : public QObject {
    Q_OBJECT
public:
    /// Construct a source producing `width`x`height` frames at `fps`. A small
    /// pool (a few buffers) is allocated up front to recycle frame memory.
    explicit MockFrameSource(int width = 640, int height = 360, int fps = 30,
                             QObject* parent = nullptr);
    ~MockFrameSource() override = default;

    /// Begin emitting frames. Returns InvalidArgument if the resolution/pool is
    /// unusable. Idempotent: a second call while running is a no-op success.
    Status start();

    /// Stop the timer; safe to call when already stopped.
    void stop();

    /// Synthesize and return exactly one frame immediately (does not require the
    /// timer to be running). Useful for headless/offscreen rendering. Returns
    /// ResourceExhausted if no pool buffer is free.
    [[nodiscard]] Result<VideoFrame, Error> generateOne();

    [[nodiscard]] int width() const noexcept { return width_; }
    [[nodiscard]] int height() const noexcept { return height_; }

signals:
    /// Emitted once per tick with a freshly generated frame.
    void frameReady(const vc::VideoFrame& frame);

private:
    void tick();
    void fillPattern(VideoFrame& frame, uint64_t phase) const;

    int width_;
    int height_;
    int fps_;
    uint64_t frameCounter_ = 0;
    std::shared_ptr<FramePool> pool_;
    QTimer timer_;
};

} // namespace vc::ui
