// Round-trip tests for the codec module.
//
//  (a) Video: synthetic I420 gradient/color-bar frame -> encode -> decode.
//      Asserts dimensions survive and reconstruction PSNR exceeds ~30 dB
//      (H.264 is lossy, so we do NOT require exact equality).
//  (b) Keyframes: the first encoded frame is a keyframe; forceKeyframe() makes
//      a mid-stream frame an IDR.
//  (c) Audio: synthetic sine PCM -> Opus encode -> decode, asserting the sample
//      count round-trips and the signal energy is roughly preserved.

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "codec/AudioDecoder.h"
#include "codec/AudioEncoder.h"
#include "codec/VideoDecoder.h"
#include "codec/VideoEncoder.h"
#include "common/AudioFrame.h"
#include "common/Clock.h"
#include "common/FramePool.h"
#include "common/Types.h"
#include "common/VideoFrame.h"

namespace {

using namespace vc;

// Fill an I420 frame with a deterministic gradient + color bars so the encoder
// has real structure to compress (a flat frame is trivially lossless).
void fillSynthetic(VideoFrame& f) {
    const int w = f.width;
    const int h = f.height;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = f.planes[0].data + static_cast<std::size_t>(y) * f.planes[0].stride;
        for (int x = 0; x < w; ++x) {
            // Diagonal gradient with vertical color-bar banding.
            row[x] = static_cast<uint8_t>((x * 255 / w + y * 255 / h) / 2 +
                                          ((x / 32) * 17 % 64));
        }
    }
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    for (int y = 0; y < ch; ++y) {
        uint8_t* u = f.planes[1].data + static_cast<std::size_t>(y) * f.planes[1].stride;
        uint8_t* v = f.planes[2].data + static_cast<std::size_t>(y) * f.planes[2].stride;
        for (int x = 0; x < cw; ++x) {
            u[x] = static_cast<uint8_t>(64 + (x * 128 / cw));
            v[x] = static_cast<uint8_t>(192 - (y * 128 / ch));
        }
    }
}

double psnrLuma(const VideoFrame& a, const VideoFrame& b) {
    const int w = a.width;
    const int h = a.height;
    double mse = 0.0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* ra = a.planes[0].data + static_cast<std::size_t>(y) * a.planes[0].stride;
        const uint8_t* rb = b.planes[0].data + static_cast<std::size_t>(y) * b.planes[0].stride;
        for (int x = 0; x < w; ++x) {
            const double d = static_cast<double>(ra[x]) - rb[x];
            mse += d * d;
        }
    }
    mse /= static_cast<double>(w) * h;
    if (mse <= 0.0) return 99.0; // perfect
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

VideoFrame makeFrame(const std::shared_ptr<FramePool>& pool, int w, int h) {
    auto buf = pool->acquire();
    EXPECT_TRUE(buf != nullptr);
    VideoFrame f = VideoFrame::makeI420(std::move(buf), w, h, MediaClock::now());
    EXPECT_TRUE(f.valid());
    fillSynthetic(f);
    return f;
}

TEST(VideoCodec, RoundTripPsnr) {
    constexpr int kW = 320, kH = 240;
    auto pool = FramePool::create(VideoFrame::i420Size(kW, kH), 4);

    VideoEncoder enc;
    ASSERT_TRUE(enc.open({kW, kH, 30, 1500}));
    VideoDecoder dec;
    ASSERT_TRUE(dec.open());

    VideoFrame in = makeFrame(pool, kW, kH);
    in.rtpTimestamp = 12345;

    auto encRes = enc.encode(in);
    ASSERT_TRUE(encRes) << "encode failed: " << to_string(encRes.error());
    const EncodedFrame& ef = encRes.value();
    EXPECT_GT(ef.size(), 0u);
    EXPECT_EQ(ef.videoCodec, VideoCodec::H264);
    EXPECT_EQ(ef.rtpTimestamp, 12345u);

    auto decRes = dec.decode(ef);
    ASSERT_TRUE(decRes) << "decode failed: " << to_string(decRes.error());
    const VideoFrame& out = decRes.value();
    EXPECT_EQ(out.width, kW);
    EXPECT_EQ(out.height, kH);
    EXPECT_EQ(out.format, PixelFormat::I420);
    EXPECT_EQ(out.rtpTimestamp, 12345u);

    const double psnr = psnrLuma(in, out);
    std::printf("[ video PSNR ] %.2f dB\n", psnr);
    EXPECT_GT(psnr, 30.0) << "reconstruction PSNR too low: " << psnr;
}

TEST(VideoCodec, KeyframeBehavior) {
    constexpr int kW = 320, kH = 240;
    auto pool = FramePool::create(VideoFrame::i420Size(kW, kH), 4);

    VideoEncoder enc;
    ASSERT_TRUE(enc.open({kW, kH, 30, 1500}));

    // The very first encoded frame must be a keyframe (IDR).
    {
        VideoFrame f = makeFrame(pool, kW, kH);
        auto r = enc.encode(f);
        ASSERT_TRUE(r);
        EXPECT_TRUE(r.value().keyframe) << "first frame should be a keyframe";
    }

    // Subsequent frames within the GOP are normally inter (P) frames.
    bool sawNonKey = false;
    for (int i = 0; i < 5; ++i) {
        VideoFrame f = makeFrame(pool, kW, kH);
        auto r = enc.encode(f);
        ASSERT_TRUE(r);
        if (!r.value().keyframe) sawNonKey = true;
    }
    EXPECT_TRUE(sawNonKey) << "expected at least one P-frame after the IDR";

    // forceKeyframe() must make the next frame an IDR.
    enc.forceKeyframe();
    {
        VideoFrame f = makeFrame(pool, kW, kH);
        auto r = enc.encode(f);
        ASSERT_TRUE(r);
        EXPECT_TRUE(r.value().keyframe) << "forceKeyframe() should yield an IDR";
    }
}

TEST(AudioCodec, SineRoundTrip) {
    AudioFormat fmt;
    fmt.sampleRateHz = 48000;
    fmt.channels = 1;
    constexpr int kSamples = 960; // 20 ms @ 48 kHz

    AudioEncoder enc;
    ASSERT_TRUE(enc.open(fmt, 64000));
    AudioDecoder dec;
    ASSERT_TRUE(dec.open(fmt));

    auto pool = FramePool::create(
        static_cast<std::size_t>(kSamples) * fmt.channels * sizeof(int16_t), 4);

    // Opus has ~6.5 ms of algorithmic look-ahead, so the FIRST decoded frame is
    // attenuated/warming up. Push several continuous 20 ms frames of a steady
    // 440 Hz sine through and assess energy on the LAST (steady-state) frame.
    constexpr int kFrames = 5;
    double inEnergy = 0.0;
    double outEnergy = 0.0;
    int lastCount = 0;
    uint32_t lastTs = 0;

    for (int frame = 0; frame < kFrames; ++frame) {
        auto buf = pool->acquire();
        ASSERT_TRUE(buf != nullptr);
        AudioFrame in = AudioFrame::makePcm(std::move(buf), fmt, kSamples, MediaClock::now());
        ASSERT_TRUE(in.valid());
        in.rtpTimestamp = static_cast<uint32_t>(4242 + frame * kSamples);

        double frameInEnergy = 0.0;
        for (int i = 0; i < kSamples; ++i) {
            const int phase = frame * kSamples + i;
            const double s = std::sin(2.0 * M_PI * 440.0 * phase / fmt.sampleRateHz);
            const int16_t v = static_cast<int16_t>(s * 16000.0);
            in.samples[i] = v;
            frameInEnergy += static_cast<double>(v) * v;
        }

        auto encRes = enc.encode(in);
        ASSERT_TRUE(encRes) << "opus encode failed: " << to_string(encRes.error());
        EXPECT_GT(encRes.value().size(), 0u);
        EXPECT_EQ(encRes.value().audioCodec, AudioCodec::Opus);
        EXPECT_EQ(encRes.value().rtpTimestamp, in.rtpTimestamp);

        auto decRes = dec.decode(encRes.value());
        ASSERT_TRUE(decRes) << "opus decode failed: " << to_string(decRes.error());
        const AudioFrame& out = decRes.value();
        EXPECT_EQ(out.format.channels, 1);

        double frameOutEnergy = 0.0;
        for (int i = 0; i < out.sampleCount; ++i) {
            frameOutEnergy += static_cast<double>(out.samples[i]) * out.samples[i];
        }

        // Keep only the final (warmed-up) frame for the energy comparison.
        inEnergy = frameInEnergy;
        outEnergy = frameOutEnergy;
        lastCount = out.sampleCount;
        lastTs = out.rtpTimestamp;
    }

    EXPECT_EQ(lastCount, kSamples);
    EXPECT_EQ(lastTs, static_cast<uint32_t>(4242 + (kFrames - 1) * kSamples));

    // Opus is lossy but steady-state per-frame energy should be the same order
    // of magnitude (allow a generous band).
    const double ratio = outEnergy / inEnergy;
    std::printf("[ audio energy ratio ] %.3f\n", ratio);
    EXPECT_GT(ratio, 0.5);
    EXPECT_LT(ratio, 2.0);
}

} // namespace
