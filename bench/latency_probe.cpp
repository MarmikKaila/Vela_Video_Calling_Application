// latency_probe — measures real per-stage latency of the media pipeline on this
// machine, with no camera or network. It pushes synthetic frames through the
// actual codecs, RTP (de)packetizer, and jitter buffer, timing each stage with
// a steady clock and reporting mean / p50 / p90 / p99 (microseconds).
//
// Stages measured:
//   video: H.264 encode -> RTP packetize -> JitterBuffer push+pop (incl.
//          depacketize) -> H.264 decode, and their sum (codec+RTP path).
//   audio: Opus encode -> Opus decode.
//
// This is the processing latency of the pipeline; it deliberately excludes the
// configurable jitter playout delay (set to 0 here) and real capture/render/
// network time, which are reported separately in observations/.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "codec/AudioDecoder.h"
#include "codec/AudioEncoder.h"
#include "codec/VideoDecoder.h"
#include "codec/VideoEncoder.h"
#include "common/AudioFrame.h"
#include "common/FramePool.h"
#include "common/VideoFrame.h"
#include "metrics/Metrics.h"
#include "network/JitterBuffer.h"
#include "network/RTPHandler.h"

using namespace vc;
using vc::metrics::LatencyHistogram;
using clock_t_ = std::chrono::steady_clock;

namespace {

uint64_t usSince(clock_t_::time_point t0) {
    return std::chrono::duration_cast<std::chrono::microseconds>(clock_t_::now() - t0).count();
}

void printRow(const char* name, const LatencyHistogram& h) {
    std::printf("  %-22s mean=%7.1f  p50=%7.1f  p90=%7.1f  p99=%7.1f\n", name,
                h.mean(), h.percentile(0.50), h.percentile(0.90), h.percentile(0.99));
}

// Fill an I420 frame with a moving gradient so the encoder has real content.
void fillFrame(VideoFrame& f, int seed) {
    for (int y = 0; y < f.height; ++y) {
        uint8_t* row = f.planes[0].data + y * f.planes[0].stride;
        for (int x = 0; x < f.width; ++x) row[x] = static_cast<uint8_t>((x + y + seed) & 0xFF);
    }
    const int cw = (f.width + 1) / 2, ch = (f.height + 1) / 2;
    for (int p = 1; p <= 2; ++p)
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                f.planes[p].data[y * f.planes[p].stride + x] = static_cast<uint8_t>(128 + ((x + seed) & 0x3F));
}

}  // namespace

int main(int argc, char** argv) {
    const int kW = 640, kH = 480;
    const int frames = argc > 1 ? std::atoi(argv[1]) : 300;

    VideoEncoder venc;
    VideoDecoder vdec;
    VideoEncoder::Config vc;
    vc.width = kW; vc.height = kH; vc.fps = 30; vc.bitrateKbps = 1500;
    if (!venc.open(vc) || !vdec.open()) { std::fprintf(stderr, "codec open failed\n"); return 1; }

    auto pool = FramePool::create(VideoFrame::i420Size(kW, kH), 8);
    RTPHandler rtp(96, 0x1234, kVideoRtpClockHz);

    LatencyHistogram hEnc(50, 100000), hPkt(20, 50000), hJit(20, 50000), hDec(50, 100000), hTot(50, 200000);

    int counted = 0;
    for (int i = 0; i < frames; ++i) {
        auto buf = pool->acquire();
        if (!buf) continue;
        VideoFrame f = VideoFrame::makeI420(buf, kW, kH, MediaClock::now());
        f.rtpTimestamp = static_cast<uint32_t>(i) * 3000u;
        fillFrame(f, i);

        auto t0 = clock_t_::now();
        auto enc = venc.encode(f);
        const uint64_t encUs = usSince(t0);
        if (!enc || enc.value().empty()) continue;  // encoder warm-up / buffered

        auto t1 = clock_t_::now();
        auto pkts = rtp.packetize(enc.value());
        const uint64_t pktUs = usSince(t1);
        if (!pkts) continue;

        // Jitter buffer with zero playout delay: measures processing only.
        JitterBufferConfig jc; jc.targetDelay = std::chrono::milliseconds(0);
        jc.kind = MediaKind::Video; jc.videoCodec = VideoCodec::H264;
        JitterBuffer jb(jc);
        const auto now = MediaClock::now();
        for (const auto& p : pkts.value()) jb.push(p, now);

        auto t2 = clock_t_::now();
        auto ef = jb.pop(now);
        const uint64_t jitUs = usSince(t2);
        if (!ef) continue;

        auto t3 = clock_t_::now();
        auto dec = vdec.decode(*ef);
        const uint64_t decUs = usSince(t3);
        if (!dec) continue;

        hEnc.record(encUs); hPkt.record(pktUs); hJit.record(jitUs); hDec.record(decUs);
        hTot.record(encUs + pktUs + jitUs + decUs);
        ++counted;
    }

    // --- Audio (Opus) -------------------------------------------------------
    AudioFormat af; af.sampleRateHz = 48000; af.channels = 1;
    AudioEncoder aenc; AudioDecoder adec;
    LatencyHistogram hAEnc(20, 50000), hADec(20, 50000);
    int aCounted = 0;
    if (aenc.open(af, 32000) && adec.open(af)) {
        auto apool = FramePool::create(960 * sizeof(int16_t), 8);
        for (int i = 0; i < frames; ++i) {
            auto buf = apool->acquire();
            if (!buf) continue;
            AudioFrame a = AudioFrame::makePcm(buf, af, 960, MediaClock::now());
            for (int s = 0; s < 960; ++s) a.samples[s] = static_cast<int16_t>(3000 * ((s + i) % 32 - 16));
            a.rtpTimestamp = static_cast<uint32_t>(i) * 960u;

            auto t0 = clock_t_::now();
            auto e = aenc.encode(a);
            const uint64_t eUs = usSince(t0);
            if (!e || e.value().empty()) continue;
            auto t1 = clock_t_::now();
            auto d = adec.decode(e.value());
            const uint64_t dUs = usSince(t1);
            if (!d) continue;
            hAEnc.record(eUs); hADec.record(dUs);
            ++aCounted;
        }
    }

    std::printf("Pipeline latency probe — %dx%d H.264, 48 kHz Opus (microseconds)\n", kW, kH);
    std::printf("Video frames measured: %d\n", counted);
    printRow("H.264 encode", hEnc);
    printRow("RTP packetize", hPkt);
    printRow("JitterBuffer push+pop", hJit);
    printRow("H.264 decode", hDec);
    printRow("VIDEO codec+RTP total", hTot);
    std::printf("Audio frames measured: %d\n", aCounted);
    printRow("Opus encode", hAEnc);
    printRow("Opus decode", hADec);
    return 0;
}
