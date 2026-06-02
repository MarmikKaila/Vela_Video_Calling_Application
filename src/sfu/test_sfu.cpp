// SFU unit tests: forwarding topology, membership limits, keyframe-on-join,
// and REMB-driven target bitrate.

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

#include "network/RtpPacket.h"
#include "sfu/Room.h"
#include "sfu/SFUServer.h"

using namespace vc;

namespace {

// A sink that records how many packets it received and the last SSRC, so a test
// can assert who a packet reached.
struct RecordingSink {
    std::shared_ptr<std::atomic<int>> count = std::make_shared<std::atomic<int>>(0);
    PacketSink fn() {
        auto c = count;
        return [c](const RtpPacket&) { c->fetch_add(1); };
    }
    int n() const { return count->load(); }
};

RtpPacket videoPacket(uint32_t ssrc, uint16_t seq) {
    RtpPacket p;
    p.ssrc = ssrc;
    p.sequenceNumber = seq;
    p.payloadType = 96;
    p.payload = {0x01, 0x02, 0x03};
    return p;
}

} // namespace

TEST(SFU, ForwardsToOthersNotSender) {
    SFUServer sfu;
    RecordingSink a, b, c;
    auto pa = sfu.addParticipant("room", "A", a.fn());
    auto pb = sfu.addParticipant("room", "B", b.fn());
    auto pc = sfu.addParticipant("room", "C", c.fn());
    ASSERT_TRUE(pa && pb && pc);

    sfu.routePacket("room", "A", videoPacket(0xA, 1));

    EXPECT_EQ(a.n(), 0); // never echoed to sender
    EXPECT_EQ(b.n(), 1);
    EXPECT_EQ(c.n(), 1);
}

TEST(SFU, LeaveStopsDelivery) {
    SFUServer sfu;
    RecordingSink a, b;
    sfu.addParticipant("room", "A", a.fn());
    sfu.addParticipant("room", "B", b.fn());

    ASSERT_TRUE(sfu.removeParticipant("room", "B"));
    sfu.routePacket("room", "A", videoPacket(0xA, 1));
    EXPECT_EQ(b.n(), 0); // B left, receives nothing
}

TEST(SFU, NinthParticipantRejected) {
    Room room("room");
    for (int i = 0; i < 8; ++i) {
        auto r = room.add("p" + std::to_string(i), {});
        ASSERT_TRUE(r.has_value());
    }
    auto overflow = room.add("p8", {});
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error(), Error::ResourceExhausted);
    EXPECT_EQ(room.size(), 8u);
}

TEST(SFU, DuplicateIdRejected) {
    Room room("room");
    ASSERT_TRUE(room.add("A", {}).has_value());
    auto dup = room.add("A", {});
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error(), Error::InvalidArgument);
}

TEST(SFU, KeyframeRequestedFromExistingSendersOnJoin) {
    std::vector<ParticipantId> requested;
    Room room("room", [&](const ParticipantId& sender) { requested.push_back(sender); });

    // A joins and registers as a video sender.
    auto a = room.add("A", {});
    ASSERT_TRUE(a.has_value());
    a.value()->setVideoSsrc(0xAAAA);
    EXPECT_TRUE(requested.empty()); // no existing senders when A joined

    // B joins: A is an existing video sender, so a keyframe is requested from A.
    auto b = room.add("B", {});
    ASSERT_TRUE(b.has_value());
    ASSERT_EQ(requested.size(), 1u);
    EXPECT_EQ(requested[0], "A");
    EXPECT_TRUE(b.value()->needsKeyframe());
}

TEST(SFU, RembDrivesTargetBitrateToReceiverMinimum) {
    SFUServer sfu;
    sfu.addParticipant("room", "A", {}); // sender
    sfu.addParticipant("room", "B", {}); // receiver
    sfu.addParticipant("room", "C", {}); // receiver

    sfu.onRemb("room", "B", 2'000'000); // B can receive 2 Mbps
    sfu.onRemb("room", "C", 800'000);   // C only 800 kbps

    // A's target is bounded by its slowest receiver (C).
    EXPECT_EQ(sfu.targetBitrate("room", "A"), 800'000u);
    // C's target is bounded by its receivers (A has none reported, B=2Mbps) => B.
    EXPECT_EQ(sfu.targetBitrate("room", "C"), 2'000'000u);
}
