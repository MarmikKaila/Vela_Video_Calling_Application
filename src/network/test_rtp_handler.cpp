// Tests for RtpPacket (serialize/parse) and RTPHandler (H.264 single-NAL +
// FU-A fragmentation/reassembly, Opus, marker bit).

#include <gtest/gtest.h>

#include <cstdint>
#include <numeric>
#include <vector>

#include "common/EncodedFrame.h"
#include "common/Types.h"
#include "network/RTPHandler.h"
#include "network/RtpPacket.h"

using namespace vc;

namespace {

// Build an Annex-B NAL unit: optional 4-byte start code is added by the caller;
// here we make a NAL body of `len` bytes with header byte `nalHeader`.
std::vector<uint8_t> makeNal(uint8_t nalHeader, std::size_t bodyLen, uint8_t fill) {
    std::vector<uint8_t> nal;
    nal.push_back(nalHeader);
    nal.insert(nal.end(), bodyLen, fill);
    return nal;
}

void appendAnnexB(std::vector<uint8_t>& out, const std::vector<uint8_t>& nal) {
    out.insert(out.end(), {0x00, 0x00, 0x00, 0x01});
    out.insert(out.end(), nal.begin(), nal.end());
}

} // namespace

TEST(RtpPacket, SerializeParseRoundTrip) {
    RtpPacket p;
    p.marker = true;
    p.padding = false;
    p.payloadType = 96;
    p.sequenceNumber = 0xBEEF;
    p.timestamp = 0x12345678;
    p.ssrc = 0xDEADBEEF;
    p.csrcs = {0x11112222, 0x33334444};
    p.payload = {1, 2, 3, 4, 5, 6, 7};

    const auto bytes = p.serialize();
    // 12 fixed + 2 CSRC * 4 + 7 payload = 27.
    EXPECT_EQ(bytes.size(), 12u + 8u + 7u);

    auto parsed = RtpPacket::parse(bytes);
    ASSERT_TRUE(parsed);
    const RtpPacket& q = parsed.value();
    EXPECT_EQ(q.version, 2);
    EXPECT_TRUE(q.marker);
    EXPECT_EQ(q.payloadType, 96);
    EXPECT_EQ(q.sequenceNumber, 0xBEEF);
    EXPECT_EQ(q.timestamp, 0x12345678u);
    EXPECT_EQ(q.ssrc, 0xDEADBEEFu);
    ASSERT_EQ(q.csrcs.size(), 2u);
    EXPECT_EQ(q.csrcs[0], 0x11112222u);
    EXPECT_EQ(q.csrcs[1], 0x33334444u);
    EXPECT_EQ(q.payload, p.payload);
}

TEST(RtpPacket, NetworkByteOrder) {
    RtpPacket p;
    p.sequenceNumber = 0x0102;
    p.timestamp = 0x03040506;
    p.ssrc = 0x0708090A;
    const auto b = p.serialize();
    // seq at offset 2..3, ts at 4..7, ssrc at 8..11, big-endian.
    EXPECT_EQ(b[2], 0x01);
    EXPECT_EQ(b[3], 0x02);
    EXPECT_EQ(b[4], 0x03);
    EXPECT_EQ(b[5], 0x04);
    EXPECT_EQ(b[6], 0x05);
    EXPECT_EQ(b[7], 0x06);
    EXPECT_EQ(b[8], 0x07);
    EXPECT_EQ(b[9], 0x08);
    EXPECT_EQ(b[10], 0x09);
    EXPECT_EQ(b[11], 0x0A);
}

TEST(RtpPacket, ParseRejectsTruncatedAndBadVersion) {
    std::vector<uint8_t> tooShort(8, 0);
    EXPECT_FALSE(RtpPacket::parse(tooShort));

    RtpPacket p;
    auto bytes = p.serialize();
    bytes[0] = static_cast<uint8_t>(bytes[0] & 0x3F); // version -> 0
    EXPECT_FALSE(RtpPacket::parse(bytes));
}

TEST(RTPHandler, H264SmallNalSinglePacketMode) {
    EncodedFrame frame;
    frame.kind = MediaKind::Video;
    frame.videoCodec = VideoCodec::H264;
    frame.rtpTimestamp = 9000;

    std::vector<uint8_t> annexb;
    appendAnnexB(annexb, makeNal(0x65, 40, 0xAB)); // IDR slice, 41-byte NAL
    frame.setPayload(annexb);

    RTPHandler h(96, 0xCAFE, kVideoRtpClockHz, /*maxPayload=*/1200);
    auto pkts = h.packetize(frame);
    ASSERT_TRUE(pkts);
    ASSERT_EQ(pkts.value().size(), 1u);
    const auto& p = pkts.value()[0];
    EXPECT_TRUE(p.marker); // last (and only) packet of the AU
    EXPECT_EQ(p.timestamp, 9000u);
    EXPECT_EQ(p.payloadType, 96);
    EXPECT_EQ(p.ssrc, 0xCAFEu);
    // Payload is the NAL verbatim (no start code).
    EXPECT_EQ(p.payload.size(), 41u);
    EXPECT_EQ(p.payload[0], 0x65);

    auto back = RTPHandler::depacketize(pkts.value(), MediaKind::Video);
    ASSERT_TRUE(back);
    EXPECT_TRUE(back.value().keyframe);
    EXPECT_EQ(back.value().rtpTimestamp, 9000u);
    std::vector<uint8_t> got(back.value().data(), back.value().data() + back.value().size());
    EXPECT_EQ(got, annexb);
}

TEST(RTPHandler, H264LargeNalFuAFragmentReassemble) {
    EncodedFrame frame;
    frame.kind = MediaKind::Video;
    frame.videoCodec = VideoCodec::H264;
    frame.rtpTimestamp = 18000;

    // One large non-IDR NAL whose body forces FU-A fragmentation.
    std::vector<uint8_t> body(5000);
    std::iota(body.begin(), body.end(), uint8_t{0});
    std::vector<uint8_t> nal;
    nal.push_back(0x41); // non-IDR slice (type 1, NRI=2)
    nal.insert(nal.end(), body.begin(), body.end());

    std::vector<uint8_t> annexb;
    appendAnnexB(annexb, nal);
    frame.setPayload(annexb);

    const std::size_t kMtu = 1200;
    RTPHandler h(96, 1, kVideoRtpClockHz, kMtu);
    auto pkts = h.packetize(frame);
    ASSERT_TRUE(pkts);
    const auto& v = pkts.value();
    ASSERT_GT(v.size(), 1u); // fragmented

    // All but the last carry no marker; only the last AU packet has marker.
    for (std::size_t i = 0; i + 1 < v.size(); ++i) EXPECT_FALSE(v[i].marker) << i;
    EXPECT_TRUE(v.back().marker);

    // Each FU-A packet: indicator type 28, fits MTU.
    for (const auto& p : v) {
        ASSERT_GE(p.payload.size(), 2u);
        EXPECT_EQ(p.payload[0] & 0x1F, 28);
        EXPECT_LE(p.payload.size(), kMtu);
    }
    // First has S bit, last has E bit.
    EXPECT_TRUE(v.front().payload[1] & 0x80);
    EXPECT_FALSE(v.front().payload[1] & 0x40);
    EXPECT_TRUE(v.back().payload[1] & 0x40);
    EXPECT_FALSE(v.back().payload[1] & 0x80);

    auto back = RTPHandler::depacketize(v, MediaKind::Video);
    ASSERT_TRUE(back);
    std::vector<uint8_t> got(back.value().data(), back.value().data() + back.value().size());
    EXPECT_EQ(got, annexb);
    EXPECT_FALSE(back.value().keyframe);
}

TEST(RTPHandler, H264MultiNalAccessUnitMarkerOnLast) {
    EncodedFrame frame;
    frame.kind = MediaKind::Video;
    frame.videoCodec = VideoCodec::H264;
    frame.rtpTimestamp = 27000;

    std::vector<uint8_t> annexb;
    appendAnnexB(annexb, makeNal(0x67, 10, 0x01)); // SPS
    appendAnnexB(annexb, makeNal(0x68, 4, 0x02));  // PPS
    appendAnnexB(annexb, makeNal(0x65, 50, 0x03)); // IDR
    frame.setPayload(annexb);

    RTPHandler h(96, 7);
    auto pkts = h.packetize(frame);
    ASSERT_TRUE(pkts);
    ASSERT_EQ(pkts.value().size(), 3u);
    EXPECT_FALSE(pkts.value()[0].marker);
    EXPECT_FALSE(pkts.value()[1].marker);
    EXPECT_TRUE(pkts.value()[2].marker);
    // Sequence numbers increment.
    EXPECT_EQ(static_cast<uint16_t>(pkts.value()[1].sequenceNumber -
                                    pkts.value()[0].sequenceNumber), 1);

    auto back = RTPHandler::depacketize(pkts.value(), MediaKind::Video);
    ASSERT_TRUE(back);
    EXPECT_TRUE(back.value().keyframe);
    std::vector<uint8_t> got(back.value().data(), back.value().data() + back.value().size());
    EXPECT_EQ(got, annexb);
}

TEST(RTPHandler, OpusOnePacketPerFrame) {
    EncodedFrame frame;
    frame.kind = MediaKind::Audio;
    frame.audioCodec = AudioCodec::Opus;
    frame.rtpTimestamp = 960;
    std::vector<uint8_t> opus = {0x10, 0x20, 0x30, 0x40, 0x50};
    frame.setPayload(opus);

    RTPHandler h(111, 0xABCD, kAudioRtpClockHz);
    auto pkts = h.packetize(frame);
    ASSERT_TRUE(pkts);
    ASSERT_EQ(pkts.value().size(), 1u);
    EXPECT_TRUE(pkts.value()[0].marker);
    EXPECT_EQ(pkts.value()[0].payload, opus);

    auto back = RTPHandler::depacketize(pkts.value(), MediaKind::Audio);
    ASSERT_TRUE(back);
    std::vector<uint8_t> got(back.value().data(), back.value().data() + back.value().size());
    EXPECT_EQ(got, opus);
    EXPECT_EQ(back.value().rtpTimestamp, 960u);
}

TEST(RTPHandler, RejectsEmptyAndUnsupported) {
    RTPHandler h(96, 1);
    EncodedFrame empty;
    empty.kind = MediaKind::Video;
    empty.videoCodec = VideoCodec::H264;
    EXPECT_FALSE(h.packetize(empty));

    EncodedFrame unsup;
    unsup.kind = MediaKind::Video;
    unsup.videoCodec = VideoCodec::Unknown;
    unsup.setPayload(std::vector<uint8_t>{0x00, 0x00, 0x00, 0x01, 0x65});
    EXPECT_FALSE(h.packetize(unsup));
}
