// Compiles and lightly exercises every shared contract header. This is not a
// real test — it exists so CI fails loudly if a change breaks a cross-module
// seam that downstream phases depend on.

#include <cstdio>

#include "capture/CaptureDevice.h"
#include "common/AudioFrame.h"
#include "common/Clock.h"
#include "common/EncodedFrame.h"
#include "common/Error.h"
#include "common/FramePool.h"
#include "common/Result.h"
#include "common/Types.h"
#include "common/VideoFrame.h"

using namespace vc;

static Result<int, Error> parsePositive(int x) {
    if (x <= 0) return fail(Error::InvalidArgument);
    return x;
}

int main() {
    // Result / Status
    auto r = parsePositive(7);
    if (!r || r.value() != 7) return 1;
    if (parsePositive(-1)) return 2;
    Status s = ok();
    if (!s) return 3;

    // FramePool round-trip with VideoFrame
    constexpr int w = 320, h = 240;
    auto pool = FramePool::create(VideoFrame::i420Size(w, h), 4);
    if (pool->available() != 4) return 4;
    {
        auto buf = pool->acquire();
        auto frame = VideoFrame::makeI420(buf, w, h, MediaClock::now());
        if (!frame.valid()) return 5;
        frame.rtpTimestamp = MediaClock::toRtp(frame.captureTime, frame.captureTime, kVideoRtpClockHz);
        if (pool->available() != 3) return 6; // one buffer checked out
    }
    if (pool->available() != 4) return 7; // returned on frame destruction

    // EncodedFrame payload sharing
    EncodedFrame ef;
    ef.kind = MediaKind::Video;
    ef.videoCodec = VideoCodec::H264;
    ef.keyframe = true;
    ef.setPayload({0, 0, 0, 1, 0x67});
    if (ef.size() != 5 || !ef.keyframe) return 8;

    // NTP mapping sanity
    auto ntp = MediaClock::toNtp(MediaClock::wallNow());
    if (ntp.seconds == 0) return 9;

    std::puts("contracts_selfcheck: OK");
    return 0;
}
