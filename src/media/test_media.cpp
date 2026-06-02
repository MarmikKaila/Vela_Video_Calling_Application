// Media-pipeline tests. The headline is an end-to-end loopback that proves the
// 1:1 media path works with no camera and no network: a synthetic I420 frame is
// encoded, packetized to RTP, run through the jitter buffer, depacketized and
// decoded back to an I420 frame. Plus AVSync mapping and encoder passthrough.

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

#include "common/Clock.h"
#include "common/FramePool.h"
#include "common/VideoFrame.h"
#include "media/AVSync.h"
#include "media/MediaPipeline.h"

using namespace vc;
using namespace std::chrono_literals;

namespace {

constexpr int kW = 320, kH = 240;

// Build a synthetic I420 frame (a simple gradient) from a pool buffer.
VideoFrame makeFrame(FramePool& pool, uint32_t rtpTs, SteadyTime t) {
    auto buf = pool.acquire();
    EXPECT_TRUE(buf != nullptr);
    VideoFrame f = VideoFrame::makeI420(buf, kW, kH, t);
    EXPECT_TRUE(f.valid());
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            f.planes[0].data[y * f.planes[0].stride + x] =
                static_cast<uint8_t>((x + y) & 0xFF);
        }
    }
    const int cw = (kW + 1) / 2, ch = (kH + 1) / 2;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            f.planes[1].data[y * f.planes[1].stride + x] = 128;
            f.planes[2].data[y * f.planes[2].stride + x] = 128;
        }
    }
    f.rtpTimestamp = rtpTs;
    return f;
}

VideoEncoder::Config encoderConfig() {
    VideoEncoder::Config c;
    c.width = kW;
    c.height = kH;
    c.fps = 30;
    c.bitrateKbps = 800;
    return c;
}

} // namespace

TEST(MediaPipeline, EndToEndLoopbackDecodesFrame) {
    auto pool = FramePool::create(VideoFrame::i420Size(kW, kH), 8);

    VideoEncoder enc;
    ASSERT_TRUE(enc.open(encoderConfig()).has_value());

    std::vector<RtpPacket> wire;
    SendPipeline::Config scfg;
    scfg.videoSsrc = 0x1234;
    SendPipeline send(&enc, nullptr,
                      [&](const RtpPacket& p) { wire.push_back(p); }, scfg);

    const SteadyTime t0 = MediaClock::now();
    ASSERT_TRUE(send.pushVideoFrame(makeFrame(*pool, 3000, t0)).has_value());
    ASSERT_FALSE(wire.empty()); // the first frame (IDR) produced packets
    EXPECT_TRUE(wire.back().marker); // marker bit on the last packet of the AU

    VideoDecoder dec;
    ASSERT_TRUE(dec.open().has_value());

    int decoded = 0;
    VideoFrame out;
    ReceivePipeline::Config rcfg;
    rcfg.videoSsrc = 0x1234;
    rcfg.targetDelay = 50ms;
    ReceivePipeline recv(&dec, nullptr, rcfg, [&](const VideoFrame& f) {
        ++decoded;
        out = f;
    });

    for (const auto& p : wire) recv.pushPacket(p, t0);
    recv.tick(t0 + 100ms); // past the 50ms target delay

    ASSERT_EQ(decoded, 1);
    EXPECT_EQ(out.width, kW);
    EXPECT_EQ(out.height, kH);
    EXPECT_EQ(out.format, PixelFormat::I420);
}

TEST(AVSync, AlignsAudioAndVideoToSameWallInstant) {
    AVSync sync;
    NtpTimestamp anchor{3'900'000'000u, 0u}; // shared capture instant

    sync.onSenderReport(/*video ssrc*/ 1, /*rtp0*/ 100000, anchor, kVideoRtpClockHz);
    sync.onSenderReport(/*audio ssrc*/ 2, /*rtp0*/ 50000, anchor, kAudioRtpClockHz);

    // RTP timestamps that both represent anchor + 0.5s.
    const uint32_t videoTs = 100000 + kVideoRtpClockHz / 2; // +45000
    const uint32_t audioTs = 50000 + kAudioRtpClockHz / 2;  // +24000

    auto pv = sync.playoutTime(1, videoTs);
    auto pa = sync.playoutTime(2, audioTs);
    ASSERT_TRUE(pv.has_value());
    ASSERT_TRUE(pa.has_value());

    const auto skew = std::chrono::abs(
        std::chrono::duration_cast<std::chrono::microseconds>(*pv - *pa));
    EXPECT_LT(skew, 1000us); // lip-sync within 1 ms

    EXPECT_FALSE(sync.playoutTime(/*unknown*/ 99, 0).has_value());
}

TEST(MediaPipeline, ForceKeyframeAndBitratePassThroughToEncoder) {
    auto pool = FramePool::create(VideoFrame::i420Size(kW, kH), 8);
    VideoEncoder enc;
    ASSERT_TRUE(enc.open(encoderConfig()).has_value());

    SendPipeline::Config scfg;
    scfg.videoSsrc = 0x1;
    std::vector<bool> keyframes;
    SendPipeline send(&enc, nullptr, [](const RtpPacket&) {}, scfg);
    send.setEncodedTap([&](const EncodedFrame& ef) { keyframes.push_back(ef.keyframe); });

    const SteadyTime t0 = MediaClock::now();
    ASSERT_TRUE(send.pushVideoFrame(makeFrame(*pool, 3000, t0)).has_value());  // IDR
    ASSERT_TRUE(send.pushVideoFrame(makeFrame(*pool, 6000, t0)).has_value());  // P
    send.forceKeyframe();
    ASSERT_TRUE(send.pushVideoFrame(makeFrame(*pool, 9000, t0)).has_value());  // forced IDR

    ASSERT_EQ(keyframes.size(), 3u);
    EXPECT_TRUE(keyframes[0]);   // first frame is always a keyframe
    EXPECT_FALSE(keyframes[1]);  // second is a predicted frame
    EXPECT_TRUE(keyframes[2]);   // forceKeyframe() took effect

    // Bitrate passthrough is observable via the encoder's config.
    send.setVideoBitrate(1500);
    EXPECT_EQ(enc.config().bitrateKbps, 1500);
}
