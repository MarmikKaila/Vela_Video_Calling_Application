// sfu_benchmark — measure the SFU's packet-forwarding throughput and derive the
// headline capacity number: max concurrent conference sessions per CPU core.
//
// Method: build an N-party room, then time SFUServer::routePacket() — each call
// fans one inbound encoded packet out to the other N-1 participants. We record
// per-call latency, compute sustained forward rate, then translate that into
// "rooms per core" for a realistic per-participant send rate.
//
// This is a single-threaded microbenchmark of the forwarding core (the part that
// must scale); real deployments parallelise rooms across cores, so rooms/core is
// the meaningful unit.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "metrics/Log.h"
#include "metrics/Metrics.h"
#include "network/RtpPacket.h"
#include "sfu/SFUServer.h"

using namespace vc;
using Clock = std::chrono::steady_clock;

namespace {

// A ~1100-byte video packet, roughly one MTU-sized RTP payload.
RtpPacket makePacket(uint16_t seq) {
    RtpPacket p;
    p.payloadType = 96;
    p.sequenceNumber = seq;
    p.ssrc = 0xAAAA;
    p.timestamp = seq * 3000u;
    p.marker = true;
    p.payload.assign(1100, 0x42);
    return p;
}

// Benchmark one room of `n` participants over `iterations` forwarded packets.
void benchmarkRoom(SFUServer& sfu, int n, uint64_t iterations) {
    const std::string room = "bench-" + std::to_string(n);
    std::atomic<uint64_t> delivered{0};
    for (int i = 0; i < n; ++i) {
        auto r = sfu.addParticipant(room, "p" + std::to_string(i),
                                    [&](const RtpPacket&) { delivered.fetch_add(1, std::memory_order_relaxed); });
        if (!r) { VC_ERROR("failed to add participant {}", i); return; }
    }

    metrics::LatencyHistogram hist(/*bucketUs=*/1, /*maxUs=*/1000);
    metrics::CpuSampler cpu;

    const auto t0 = Clock::now();
    for (uint64_t k = 0; k < iterations; ++k) {
        const auto c0 = Clock::now();
        sfu.routePacket(room, "p0", makePacket(static_cast<uint16_t>(k)));
        const auto c1 = Clock::now();
        hist.record(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0).count() / 1000));
    }
    const auto t1 = Clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double routePerSec = iterations / secs;
    const double deliveriesPerSec = static_cast<double>(delivered.load()) / secs;
    const double cpuPct = cpu.utilizationPercent();

    // Realistic load: each participant sends ~250 RTP pkts/s (≈1080p@2Mbps,
    // ~1100B payloads). An n-party room generates n inbound packets each fanned
    // to n-1 peers, i.e. n routePacket() calls per "packet tick".
    constexpr double kPktsPerSenderPerSec = 250.0;
    const double routePerSecPerRoom = n * kPktsPerSenderPerSec;
    const double roomsPerCore = routePerSec / routePerSecPerRoom;

    VC_INFO("n={} | route={:.2f}M/s deliveries={:.2f}M/s | p50={:.2f}us p99={:.2f}us | "
            "cpu={:.0f}% | => ~{:.0f} rooms/core",
            n, routePerSec / 1e6, deliveriesPerSec / 1e6,
            hist.percentile(0.50), hist.percentile(0.99), cpuPct, roomsPerCore);
}

} // namespace

int main() {
    vc::log::init();
    VC_INFO("SFU forwarding benchmark (single core)");

    SFUServer sfu;
    for (int n : {2, 4, 8}) {
        benchmarkRoom(sfu, n, /*iterations=*/2'000'000);
    }
    VC_INFO("done");
    return 0;
}
