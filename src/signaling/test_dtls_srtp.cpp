// DTLS-SRTP tests: two secured RtpTransports negotiate over a loopback ICE pair,
// complete the DTLS handshake, and exchange SRTP-encrypted RTP that arrives
// byte-identical after decryption. A fingerprint mismatch must be rejected
// (no SRTP keys, so the channel never carries media).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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
    p.payloadType = 96;
    p.ssrc = 0xCAFEBABE;
    p.sequenceNumber = seq;
    p.timestamp = 90000u + seq * 3000u;
    p.payload.assign(48, static_cast<uint8_t>(seq & 0xFF));
    return p;
}

bool waitUntil(std::function<bool()> pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

// Wire two transports as a loopback ICE pair (host candidates, no STUN), with
// `a` the DTLS client and `b` the DTLS server. `remoteFpForB` lets a test feed b
// a wrong fingerprint. Returns once both have exchanged ICE setup.
void connectPair(RtpTransport& a, RtpTransport& b, const std::string& remoteFpForB) {
    NATConfig lan;
    lan.stunHost = "";

    a.enableSecurity(/*asClient=*/true);
    b.enableSecurity(/*asClient=*/false);

    RtpTransport::Callbacks acb;
    acb.onLocalCandidate = [&](const std::string& c) { (void)b.addRemoteCandidate(c); };
    ASSERT_TRUE(a.initialize(lan, std::move(acb)).has_value());

    RtpTransport::Callbacks bcb;
    bcb.onLocalCandidate = [&](const std::string& c) { (void)a.addRemoteCandidate(c); };
    ASSERT_TRUE(b.initialize(lan, std::move(bcb)).has_value());

    // Exchange fingerprints (authenticates the DTLS peer) then ICE descriptions.
    a.setRemoteFingerprint(b.localFingerprint());
    b.setRemoteFingerprint(remoteFpForB);

    auto da = a.localDescription();
    auto db = b.localDescription();
    ASSERT_TRUE(da.has_value() && db.has_value());
    ASSERT_TRUE(b.setRemoteDescription(da.value()).has_value());
    ASSERT_TRUE(a.setRemoteDescription(db.value()).has_value());
    ASSERT_TRUE(a.startGathering().has_value());
    ASSERT_TRUE(b.startGathering().has_value());
}

}  // namespace

TEST(DtlsSrtp, SecureRoundTripIsByteIdentical) {
    constexpr int kPackets = 30;
    RtpTransport a, b;

    std::mutex rxMu;
    std::vector<std::vector<uint8_t>> received;

    // We must set b's onPacket before initialize, so build b manually here.
    NATConfig lan;
    lan.stunHost = "";
    a.enableSecurity(true);
    b.enableSecurity(false);

    RtpTransport::Callbacks acb;
    acb.onLocalCandidate = [&](const std::string& c) { (void)b.addRemoteCandidate(c); };
    ASSERT_TRUE(a.initialize(lan, std::move(acb)).has_value());

    RtpTransport::Callbacks bcb;
    bcb.onLocalCandidate = [&](const std::string& c) { (void)a.addRemoteCandidate(c); };
    bcb.onPacket = [&](const RtpPacket& p) {
        std::lock_guard<std::mutex> lk(rxMu);
        received.push_back(p.serialize());
    };
    ASSERT_TRUE(b.initialize(lan, std::move(bcb)).has_value());

    a.setRemoteFingerprint(b.localFingerprint());
    b.setRemoteFingerprint(a.localFingerprint());

    auto da = a.localDescription();
    auto db = b.localDescription();
    ASSERT_TRUE(da.has_value() && db.has_value());
    ASSERT_TRUE(b.setRemoteDescription(da.value()).has_value());
    ASSERT_TRUE(a.setRemoteDescription(db.value()).has_value());
    ASSERT_TRUE(a.startGathering().has_value());
    ASSERT_TRUE(b.startGathering().has_value());

    ASSERT_TRUE(waitUntil([&] { return a.secureReady() && b.secureReady(); }, 20s))
        << "DTLS-SRTP handshake did not complete";

    std::vector<std::vector<uint8_t>> sent;
    for (int i = 0; i < kPackets; ++i) {
        RtpPacket p = makePacket(static_cast<uint16_t>(2000 + i));
        sent.push_back(p.serialize());
        ASSERT_TRUE(a.send(p).has_value());
        std::this_thread::sleep_for(2ms);
    }

    ASSERT_TRUE(waitUntil(
        [&] {
            std::lock_guard<std::mutex> lk(rxMu);
            return static_cast<int>(received.size()) >= kPackets;
        },
        5s));

    std::lock_guard<std::mutex> lk(rxMu);
    ASSERT_EQ(received.size(), sent.size());
    for (std::size_t i = 0; i < sent.size(); ++i) {
        EXPECT_EQ(received[i], sent[i]) << "decrypted packet " << i << " differs";
    }
}

TEST(DtlsSrtp, FingerprintMismatchIsRejected) {
    RtpTransport a, b;
    // b is told a bogus fingerprint for the peer → it must refuse to key SRTP.
    connectPair(a, b, "sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
                      "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF");

    // a (correct fp) may become ready, but b must NOT — the mismatch fails its
    // post-handshake fingerprint check, so it never derives SRTP keys.
    const bool bReady = waitUntil([&] { return b.secureReady(); }, 8s);
    EXPECT_FALSE(bReady) << "b accepted a peer whose fingerprint did not match";
}
