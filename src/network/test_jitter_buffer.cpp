// Tests for JitterBuffer: in-order passthrough, reordering, duplicate drop,
// late drop, gap detection, 16-bit sequence wraparound, and multi-packet
// reassembly only when complete.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "common/Clock.h"
#include "common/Types.h"
#include "network/JitterBuffer.h"
#include "network/RtpPacket.h"

using namespace vc;
using namespace std::chrono_literals;

namespace {

// Make a single-NAL-unit video packet (one packet == one complete access unit).
RtpPacket videoPacket(uint16_t seq, uint32_t ts, uint8_t nalHeader, bool marker) {
    RtpPacket p;
    p.payloadType = 96;
    p.ssrc = 1;
    p.sequenceNumber = seq;
    p.timestamp = ts;
    p.marker = marker;
    p.payload = {nalHeader, 0xAA, 0xBB, 0xCC};
    return p;
}

JitterBufferConfig videoConfig(std::chrono::milliseconds delay = 50ms) {
    JitterBufferConfig c;
    c.kind = MediaKind::Video;
    c.videoCodec = VideoCodec::H264;
    c.targetDelay = delay;
    return c;
}

// A steady-clock base we can offset deterministically.
const SteadyTime kT0 = SteadyTime{} + 1000s;

} // namespace

TEST(JitterBuffer, InOrderPassthrough) {
    JitterBuffer jb(videoConfig());
    EXPECT_TRUE(jb.push(videoPacket(100, 9000, 0x65, true), kT0));
    EXPECT_TRUE(jb.push(videoPacket(101, 18000, 0x41, true), kT0));

    // Before target delay: nothing emitted.
    EXPECT_FALSE(jb.pop(kT0 + 10ms).has_value());

    auto f1 = jb.pop(kT0 + 60ms);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->rtpTimestamp, 9000u);
    auto f2 = jb.pop(kT0 + 60ms);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->rtpTimestamp, 18000u);
    EXPECT_FALSE(jb.pop(kT0 + 60ms).has_value());
    EXPECT_EQ(jb.stats().emitted, 2u);
}

TEST(JitterBuffer, OutOfOrderReordering) {
    JitterBuffer jb(videoConfig());
    // Arrive out of order: ts 18000 (seq 101) before ts 9000 (seq 100).
    EXPECT_TRUE(jb.push(videoPacket(101, 18000, 0x41, true), kT0));
    EXPECT_TRUE(jb.push(videoPacket(100, 9000, 0x65, true), kT0));

    auto f1 = jb.pop(kT0 + 60ms);
    ASSERT_TRUE(f1.has_value());
    EXPECT_EQ(f1->rtpTimestamp, 9000u); // emitted in timestamp order
    auto f2 = jb.pop(kT0 + 60ms);
    ASSERT_TRUE(f2.has_value());
    EXPECT_EQ(f2->rtpTimestamp, 18000u);
    EXPECT_GE(jb.stats().reordered, 0u);
}

TEST(JitterBuffer, DuplicateDroppedOnce) {
    JitterBuffer jb(videoConfig());
    EXPECT_TRUE(jb.push(videoPacket(100, 9000, 0x65, true), kT0));
    EXPECT_FALSE(jb.push(videoPacket(100, 9000, 0x65, true), kT0)); // dup
    EXPECT_EQ(jb.stats().duplicates, 1u);
    EXPECT_EQ(jb.stats().pushed, 1u);

    auto f = jb.pop(kT0 + 60ms);
    ASSERT_TRUE(f.has_value());
}

TEST(JitterBuffer, LatePacketDropped) {
    JitterBuffer jb(videoConfig());
    jb.push(videoPacket(100, 9000, 0x65, true), kT0);
    jb.push(videoPacket(101, 18000, 0x41, true), kT0);

    ASSERT_TRUE(jb.pop(kT0 + 60ms).has_value()); // emit ts 9000
    ASSERT_TRUE(jb.pop(kT0 + 60ms).has_value()); // emit ts 18000

    // A straggler from the already-emitted ts 9000 frame must be dropped.
    EXPECT_FALSE(jb.push(videoPacket(99, 9000, 0x65, true), kT0 + 70ms));
    EXPECT_EQ(jb.stats().late, 1u);
}

TEST(JitterBuffer, GapDetectedAndReported) {
    JitterBuffer jb(videoConfig());
    // Multi-packet frame: FU-A start (seq 10) and end (seq 13), middle missing.
    RtpPacket s;
    s.ssrc = 1; s.sequenceNumber = 10; s.timestamp = 9000;
    s.payload = {static_cast<uint8_t>((0x60) | 28), static_cast<uint8_t>(0x80 | 1), 0x01};
    RtpPacket e;
    e.ssrc = 1; e.sequenceNumber = 13; e.timestamp = 9000; e.marker = true;
    e.payload = {static_cast<uint8_t>((0x60) | 28), static_cast<uint8_t>(0x40 | 1), 0x02};

    jb.push(s, kT0);
    jb.push(e, kT0);

    auto g = jb.gaps();
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].firstMissing, 11);
    EXPECT_EQ(g[0].lastMissing, 12);
}

TEST(JitterBuffer, MultiPacketReassembledOnlyWhenComplete) {
    JitterBuffer jb(videoConfig());
    // FU-A across seq 20 (S) and 21 (E), same timestamp.
    RtpPacket s;
    s.ssrc = 1; s.sequenceNumber = 20; s.timestamp = 9000;
    s.payload = {static_cast<uint8_t>(0x60 | 28), static_cast<uint8_t>(0x80 | 1), 0xDE, 0xAD};
    RtpPacket e;
    e.ssrc = 1; e.sequenceNumber = 21; e.timestamp = 9000; e.marker = true;
    e.payload = {static_cast<uint8_t>(0x60 | 28), static_cast<uint8_t>(0x40 | 1), 0xBE, 0xEF};

    // Only the start arrives: incomplete, even after delay it should not emit a
    // real frame (it is dropped, advancing past it).
    jb.push(s, kT0);
    EXPECT_FALSE(jb.pop(kT0 + 10ms).has_value()); // delay not elapsed

    // Now provide both for a fresh buffer to confirm completeness path.
    JitterBuffer jb2(videoConfig());
    jb2.push(s, kT0);
    jb2.push(e, kT0);
    EXPECT_FALSE(jb2.pop(kT0 + 10ms).has_value()); // not yet (delay)
    auto f = jb2.pop(kT0 + 60ms);
    ASSERT_TRUE(f.has_value());
    // Reassembled NAL: header (0x61) + DEAD + BEEF, Annex-B prefixed.
    std::vector<uint8_t> got(f->data(), f->data() + f->size());
    std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x01, 0x61, 0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_EQ(got, expected);
    EXPECT_EQ(jb2.stats().emitted, 1u);
}

TEST(JitterBuffer, IncompleteVideoFrameDroppedAfterDelay) {
    JitterBuffer jb(videoConfig());
    // FU-A start only — never completed.
    RtpPacket s;
    s.ssrc = 1; s.sequenceNumber = 20; s.timestamp = 9000;
    s.payload = {static_cast<uint8_t>(0x60 | 28), static_cast<uint8_t>(0x80 | 1), 0xDE};
    jb.push(s, kT0);
    // After the delay, incomplete frame is dropped (no emission), buffer drains.
    EXPECT_FALSE(jb.pop(kT0 + 100ms).has_value());
    EXPECT_EQ(jb.frameCount(), 0u);
    EXPECT_EQ(jb.stats().emitted, 0u);
}

TEST(JitterBuffer, SequenceWraparoundHandled) {
    JitterBuffer jb(videoConfig());
    // A multi-packet frame whose sequence numbers wrap 65535 -> 0.
    RtpPacket s;
    s.ssrc = 1; s.sequenceNumber = 65535; s.timestamp = 9000;
    s.payload = {static_cast<uint8_t>(0x60 | 28), static_cast<uint8_t>(0x80 | 1), 0x11};
    RtpPacket e;
    e.ssrc = 1; e.sequenceNumber = 0; e.timestamp = 9000; e.marker = true;
    e.payload = {static_cast<uint8_t>(0x60 | 28), static_cast<uint8_t>(0x40 | 1), 0x22};

    // Arrive reversed to also exercise reorder + min/max with wrap.
    jb.push(e, kT0);
    jb.push(s, kT0);

    // No gap: 65535 then 0 are contiguous across the wrap.
    EXPECT_TRUE(jb.gaps().empty());

    auto f = jb.pop(kT0 + 60ms);
    ASSERT_TRUE(f.has_value());
    std::vector<uint8_t> got(f->data(), f->data() + f->size());
    std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x01, 0x61, 0x11, 0x22};
    EXPECT_EQ(got, expected);
}

TEST(JitterBuffer, WraparoundGapDetected) {
    JitterBuffer jb(videoConfig());
    // seq 65534 (S) ... 1 (E): 65535 and 0 are missing across the wrap.
    RtpPacket s;
    s.ssrc = 1; s.sequenceNumber = 65534; s.timestamp = 9000;
    s.payload = {static_cast<uint8_t>(0x60 | 28), static_cast<uint8_t>(0x80 | 1), 0x11};
    RtpPacket e;
    e.ssrc = 1; e.sequenceNumber = 1; e.timestamp = 9000; e.marker = true;
    e.payload = {static_cast<uint8_t>(0x60 | 28), static_cast<uint8_t>(0x40 | 1), 0x22};
    jb.push(s, kT0);
    jb.push(e, kT0);

    auto g = jb.gaps();
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0].firstMissing, 65535);
    EXPECT_EQ(g[0].lastMissing, 0);
}

TEST(JitterBuffer, AudioSinglePacketEmits) {
    JitterBufferConfig c;
    c.kind = MediaKind::Audio;
    c.audioCodec = AudioCodec::Opus;
    JitterBuffer jb(c);

    RtpPacket p;
    p.ssrc = 2; p.sequenceNumber = 5; p.timestamp = 960; p.marker = true;
    p.payload = {0x01, 0x02, 0x03};
    jb.push(p, kT0);
    auto f = jb.pop(kT0 + 60ms);
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind, MediaKind::Audio);
    std::vector<uint8_t> got(f->data(), f->data() + f->size());
    EXPECT_EQ(got, (std::vector<uint8_t>{0x01, 0x02, 0x03}));
}
