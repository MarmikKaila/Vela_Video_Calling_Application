// End-to-end loopback tests for the signaling + NAT-traversal module.
//
//  1. SignalingLoopback: spin up an in-process uWebSockets signaling server on
//     an ephemeral port, connect two SignalingClients to the same room, have
//     client A send an offer, and assert client B receives it via onOffer.
//  2. NATTraversalHostCandidates: an ICE agent gathers at least one *host*
//     candidate with no STUN server configured, proving offline operation.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <uwebsockets/App.h>

#include "NATTraversal.h"
#include "RoomManager.h"
#include "SignalingClient.h"

using namespace std::chrono_literals;
using vc::signaling::ConnectionHandle;
using vc::signaling::ParticipantId;
using vc::signaling::RoomManager;
using vc::signaling::SignalingClient;
using vc::signaling::SignalingConfig;
using vc::signaling::SignalingCallbacks;

namespace {

struct PerSocketData {};
using Ws = uWS::WebSocket<false, true, PerSocketData>;

// Runs a uWS signaling server on its own thread. Publishes the chosen
// ephemeral port and provides a thread-safe stop().
class TestServer {
public:
    // Starts the server and blocks until it is listening (or failed). Returns
    // the bound port, or 0 on failure.
    std::uint16_t start() {
        std::thread t([this] { this->run(); });
        thread_ = std::move(t);
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return ready_; });
        return port_;
    }

    void stop() {
        // Close the listen socket from the loop thread, which ends run().
        if (loop_ && listenSocket_) {
            auto* loop = loop_;
            auto* ls = listenSocket_;
            loop->defer([ls] { us_listen_socket_close(0, ls); });
        }
        if (thread_.joinable()) thread_.join();
    }

    ~TestServer() { stop(); }

private:
    void run() {
        auto manager = std::make_shared<RoomManager>(
            [](ConnectionHandle conn, std::string_view payload) {
                static_cast<Ws*>(conn)->send(payload, uWS::OpCode::TEXT);
            });

        uWS::App app;
        app.ws<PerSocketData>("/*", {
            .compression = uWS::DISABLED,
            .maxPayloadLength = 64 * 1024,
            .idleTimeout = 0,
            .message = [manager](Ws* ws, std::string_view msg, uWS::OpCode op) {
                if (op != uWS::OpCode::TEXT) return;
                auto j = nlohmann::json::parse(msg, nullptr, false);
                if (j.is_discarded() || !j.is_object()) return;
                const std::string type = j.value("type", std::string{});
                auto* conn = static_cast<ConnectionHandle>(ws);
                if (type == "join") {
                    (void)manager->join(j.value("room", std::string{}), conn);
                } else if (type == "leave") {
                    (void)manager->leave(conn);
                } else if (type == "offer" || type == "answer" || type == "ice") {
                    const ParticipantId from = manager->idFor(conn);
                    if (from == 0) return;
                    const ParticipantId to =
                        j.value("to", static_cast<ParticipantId>(0));
                    j["from"] = from;
                    (void)manager->relay(conn, type, to, j.dump());
                }
            },
            .close = [manager](Ws* ws, int, std::string_view) {
                (void)manager->leave(static_cast<ConnectionHandle>(ws));
            },
        });

        app.listen("127.0.0.1", 0, [this](us_listen_socket_t* ls) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (ls) {
                listenSocket_ = ls;
                loop_ = uWS::Loop::get();
                port_ = static_cast<std::uint16_t>(
                    us_socket_local_port(0, reinterpret_cast<us_socket_t*>(ls)));
            }
            ready_ = true;
            cv_.notify_all();
        });

        app.run();  // returns once the listen socket is closed
    }

    std::mutex mtx_;
    std::condition_variable cv_;
    bool ready_ = false;
    std::uint16_t port_ = 0;
    us_listen_socket_t* listenSocket_ = nullptr;
    uWS::Loop* loop_ = nullptr;
    std::thread thread_;
};

}  // namespace

TEST(SignalingLoopback, OfferReachesPeer) {
    TestServer server;
    const std::uint16_t port = server.start();
    ASSERT_NE(port, 0) << "server failed to bind an ephemeral port";

    std::mutex m;
    std::condition_variable cv;
    bool aJoined = false, bJoined = false;
    bool offerReceived = false;
    ParticipantId offerFrom = 0;
    std::string offerSdp;
    ParticipantId aSelf = 0;

    SignalingClient clientA, clientB;

    SignalingCallbacks cbA;
    cbA.onJoined = [&](ParticipantId self) {
        std::lock_guard<std::mutex> lk(m);
        aSelf = self;
        aJoined = true;
        cv.notify_all();
    };
    clientA.setCallbacks(std::move(cbA));

    SignalingCallbacks cbB;
    cbB.onJoined = [&](ParticipantId) {
        std::lock_guard<std::mutex> lk(m);
        bJoined = true;
        cv.notify_all();
    };
    cbB.onOffer = [&](ParticipantId from, const std::string& sdp) {
        std::lock_guard<std::mutex> lk(m);
        offerReceived = true;
        offerFrom = from;
        offerSdp = sdp;
        cv.notify_all();
    };
    clientB.setCallbacks(std::move(cbB));

    SignalingConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.room = "loopback-room";

    ASSERT_TRUE(clientA.connect(cfg)) << "client A connect failed";
    ASSERT_TRUE(clientB.connect(cfg)) << "client B connect failed";

    {
        std::unique_lock<std::mutex> lk(m);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return aJoined && bJoined; }))
            << "both clients did not receive 'joined' in time";
    }

    // Broadcast the offer to the room (to == 0): B is the only other peer.
    const std::string sdp = "v=0\r\no=- 42 2 IN IP4 127.0.0.1\r\n";
    ASSERT_TRUE(clientA.sendOffer(/*to=*/0, sdp));

    {
        std::unique_lock<std::mutex> lk(m);
        ASSERT_TRUE(cv.wait_for(lk, 5s, [&] { return offerReceived; }))
            << "client B did not receive the offer in time";
    }

    EXPECT_EQ(offerSdp, sdp);
    EXPECT_EQ(offerFrom, aSelf) << "offer 'from' should be A's server id";

    clientA.disconnect();
    clientB.disconnect();
}

TEST(NATTraversal, GathersHostCandidateOffline) {
    using vc::signaling::IceState;
    using vc::signaling::NATCallbacks;
    using vc::signaling::NATConfig;
    using vc::signaling::NATTraversal;

    std::mutex m;
    std::condition_variable cv;
    int candidateCount = 0;
    bool gatheringDone = false;

    NATTraversal nat;
    NATConfig cfg;
    cfg.stunHost = "";  // no STUN -> host candidates only, fully offline

    NATCallbacks cbs;
    cbs.onLocalCandidate = [&](const std::string&) {
        std::lock_guard<std::mutex> lk(m);
        ++candidateCount;
        cv.notify_all();
    };
    cbs.onGatheringDone = [&]() {
        std::lock_guard<std::mutex> lk(m);
        gatheringDone = true;
        cv.notify_all();
    };

    ASSERT_TRUE(nat.initialize(cfg, std::move(cbs)));
    ASSERT_TRUE(nat.startGathering());

    {
        std::unique_lock<std::mutex> lk(m);
        // Host-candidate gathering is near-instant; allow generous slack.
        ASSERT_TRUE(cv.wait_for(lk, 10s, [&] { return candidateCount >= 1; }))
            << "no host candidate gathered offline";
    }

    // The local description should be retrievable and non-empty.
    auto desc = nat.localDescription();
    ASSERT_TRUE(desc);
    EXPECT_FALSE(desc.value().empty());
    EXPECT_GE(candidateCount, 1);
}
