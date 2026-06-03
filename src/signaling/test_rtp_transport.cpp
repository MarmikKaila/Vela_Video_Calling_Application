// End-to-end test for RtpTransport: two ICE agents on the loopback interface
// negotiate a connection in-process (host candidates only, no STUN), then one
// sends a burst of RtpPackets to the other. Every packet must arrive
// byte-identical and in order.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include "network/RtpPacket.h"
#include "signaling/RtpTransport.h"

using namespace vc;
using namespace vc::signaling;
using namespace std::chrono_literals;

namespace {

RtpPacket makePacket(uint16_t seq) {
    RtpPacket p;
    p.payloadType = 96;            // H.264 dynamic PT
    p.ssrc = 0xDEADBEEF;
    p.sequenceNumber = seq;
    p.timestamp = 90000u + seq * 3000u;
    p.marker = (seq % 5 == 0);
    // Payload pattern derived from seq so a mismatch is easy to localize.
    p.payload.assign(64, static_cast<uint8_t>(seq & 0xFF));
    return p;
}

bool waitFor(std::atomic<bool>& flag, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

}  // namespace

TEST(RtpTransport, LoopbackRoundTripIsByteIdentical) {
    constexpr int kPackets = 50;

    RtpTransport a;  // sender
    RtpTransport b;  // receiver

    std::atomic<bool> aConnected{false};
    std::atomic<bool> bConnected{false};

    std::mutex rxMu;
    std::vector<std::vector<uint8_t>> received;  // serialized bytes, in arrival order

    NATConfig lanOnly;
    lanOnly.stunHost = "";  // host candidates only — no external network

    RtpTransport::Callbacks acb;
    acb.onLocalCandidate = [&](const std::string& c) { (void)b.addRemoteCandidate(c); };
    acb.onStateChanged = [&](IceState s) {
        if (s == IceState::Connected || s == IceState::Completed) aConnected = true;
    };
    ASSERT_TRUE(a.initialize(lanOnly, std::move(acb)).has_value());

    RtpTransport::Callbacks bcb;
    bcb.onLocalCandidate = [&](const std::string& c) { (void)a.addRemoteCandidate(c); };
    bcb.onStateChanged = [&](IceState s) {
        if (s == IceState::Connected || s == IceState::Completed) bConnected = true;
    };
    bcb.onPacket = [&](const RtpPacket& p) {
        std::lock_guard<std::mutex> lk(rxMu);
        received.push_back(p.serialize());
    };
    ASSERT_TRUE(b.initialize(lanOnly, std::move(bcb)).has_value());

    // Exchange ufrag/pwd descriptions (available immediately after init), then
    // gather so candidates trickle through the onLocalCandidate callbacks.
    auto descA = a.localDescription();
    auto descB = b.localDescription();
    ASSERT_TRUE(descA.has_value());
    ASSERT_TRUE(descB.has_value());
    ASSERT_TRUE(b.setRemoteDescription(descA.value()).has_value());
    ASSERT_TRUE(a.setRemoteDescription(descB.value()).has_value());

    ASSERT_TRUE(a.startGathering().has_value());
    ASSERT_TRUE(b.startGathering().has_value());

    ASSERT_TRUE(waitFor(aConnected, 15s)) << "agent A never connected";
    ASSERT_TRUE(waitFor(bConnected, 15s)) << "agent B never connected";

    std::vector<std::vector<uint8_t>> sent;
    for (int i = 0; i < kPackets; ++i) {
        RtpPacket p = makePacket(static_cast<uint16_t>(1000 + i));
        sent.push_back(p.serialize());
        ASSERT_TRUE(a.send(p).has_value());
        std::this_thread::sleep_for(2ms);  // pace so nothing is dropped on loopback
    }

    // Allow in-flight packets to drain.
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(rxMu);
            if (static_cast<int>(received.size()) >= kPackets) break;
        }
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::sleep_for(20ms);
    }

    std::lock_guard<std::mutex> lk(rxMu);
    ASSERT_EQ(received.size(), sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        EXPECT_EQ(received[i], sent[i]) << "packet " << i << " differs on the wire";
    }
}
