// sfu_smoketest — headless end-to-end check of the SFU media path.
//
// Assumes an sfu_server is already listening (the test harness starts one).
// Two clients (SignalingClient + RtpTransport each) join the same room, connect
// their ICE channel through the server, then client A sends RTP packets that the
// SFU must forward to client B. Proves signaling + ICE + SFU forwarding without
// any camera, microphone, or display.
//
//   sfu_smoketest <host> <port> <room>
// Exit 0 on success, non-zero otherwise.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "network/RtpPacket.h"
#include "signaling/DtlsSrtp.h"
#include "signaling/RtpTransport.h"
#include "signaling/SignalingClient.h"

using namespace vc;
using namespace std::chrono_literals;

namespace {

struct Client {
    std::shared_ptr<signaling::RtpTransport> transport =
        std::make_shared<signaling::RtpTransport>();
    std::shared_ptr<signaling::SignalingClient> sig =
        std::make_shared<signaling::SignalingClient>();
    std::atomic<bool> connected{false};
    std::atomic<int> received{0};

    bool start(const std::string& host, uint16_t port, const std::string& room) {
        signaling::RtpTransport::Callbacks tcbs;
        auto sigPtr = sig;
        tcbs.onLocalCandidate = [sigPtr](const std::string& c) {
            (void)sigPtr->sendIceCandidate(0, c);
        };
        tcbs.onStateChanged = [this](signaling::IceState s) {
            if (s == signaling::IceState::Connected || s == signaling::IceState::Completed)
                connected.store(true);
        };
        tcbs.onPacket = [this](const RtpPacket&) { received.fetch_add(1); };
        signaling::NATConfig cfg;
        cfg.stunHost = "";
        transport->enableSecurity(/*asClient=*/true);  // matches the secured server
        if (!transport->initialize(cfg, std::move(tcbs))) return false;
        if (!transport->startGathering()) return false;

        signaling::SignalingCallbacks scbs;
        auto tp = transport;
        scbs.onJoined = [tp, sigPtr](signaling::PeerId) {
            auto d = tp->localDescription();
            if (d) {
                (void)sigPtr->sendOffer(
                    0, signaling::DtlsSrtp::packDescription(d.value(), tp->localFingerprint()));
            }
        };
        scbs.onAnswer = [tp](signaling::PeerId, const std::string& payload) {
            std::string ice, fp;
            signaling::DtlsSrtp::unpackDescription(payload, ice, fp);
            tp->setRemoteFingerprint(fp);
            (void)tp->setRemoteDescription(ice);
        };
        scbs.onIceCandidate = [tp](signaling::PeerId, const std::string& c) {
            (void)tp->addRemoteCandidate(c);
        };
        sig->setCallbacks(std::move(scbs));

        signaling::SignalingConfig sc;
        sc.host = host;
        sc.port = port;
        sc.room = room;
        return static_cast<bool>(sig->connect(sc));
    }
};

bool waitFor(std::atomic<bool>& f, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!f.load()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(20ms);
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const uint16_t port = static_cast<uint16_t>(argc > 2 ? std::atoi(argv[2]) : 8080);
    const std::string room = argc > 3 ? argv[3] : "smoke";

    Client a, b;
    if (!a.start(host, port, room) || !b.start(host, port, room)) {
        std::fprintf(stderr, "FAIL: could not connect to signaling server\n");
        return 1;
    }
    if (!waitFor(a.connected, 20s) || !waitFor(b.connected, 20s)) {
        std::fprintf(stderr, "FAIL: ICE did not connect (a=%d b=%d)\n",
                     a.connected.load(), b.connected.load());
        return 1;
    }
    // Wait for the DTLS-SRTP handshake to complete on both legs before sending
    // (RtpTransport drops media until SRTP keys exist).
    const auto secureDeadline = std::chrono::steady_clock::now() + 20s;
    while ((!a.transport->secureReady() || !b.transport->secureReady()) &&
           std::chrono::steady_clock::now() < secureDeadline) {
        std::this_thread::sleep_for(20ms);
    }
    if (!a.transport->secureReady() || !b.transport->secureReady()) {
        std::fprintf(stderr, "FAIL: DTLS-SRTP handshake did not complete (a=%d b=%d)\n",
                     a.transport->secureReady(), b.transport->secureReady());
        return 1;
    }
    // Give the server a moment to register both participants with the SFU.
    std::this_thread::sleep_for(500ms);

    constexpr int kPackets = 20;
    for (int i = 0; i < kPackets; ++i) {
        RtpPacket p;
        p.payloadType = 96;
        p.ssrc = 0x11111111;
        p.sequenceNumber = static_cast<uint16_t>(i);
        p.timestamp = 90000u + i * 3000u;
        p.payload.assign(32, static_cast<uint8_t>(i));
        (void)a.transport->send(p);
        std::this_thread::sleep_for(5ms);
    }

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (b.received.load() < kPackets &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
    }

    a.sig->disconnect();
    b.sig->disconnect();

    const int got = b.received.load();
    if (got >= kPackets) {
        std::printf("PASS: client B received %d/%d packets forwarded by the SFU\n",
                    got, kPackets);
        return 0;
    }
    std::fprintf(stderr, "FAIL: client B received only %d/%d packets\n", got, kPackets);
    return 1;
}
