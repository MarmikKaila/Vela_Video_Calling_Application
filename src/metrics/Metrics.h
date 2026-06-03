#pragma once

// Runtime metrics for the media path: latency distribution, packet loss, and
// CPU utilisation. These are the numbers you put on a dashboard and in a
// portfolio writeup: glass-to-glass latency percentiles, loss %, CPU per core.
//
// All types are lightweight and lock-free-friendly (a single owner thread per
// instance, or guard externally). They allocate once and have O(1) updates so
// they are safe to call on the hot path.

#include <array>
#include <cstdint>
#include <vector>

namespace vc::metrics {

// Fixed-bucket latency histogram (microsecond resolution). O(1) record, O(buckets)
// percentile. Buckets are linear of width `bucketUs` up to `maxUs`; samples above
// `maxUs` fall into a single overflow bucket.
class LatencyHistogram {
public:
    // Default: 0..200ms in 1ms buckets — appropriate for <150ms glass-to-glass.
    explicit LatencyHistogram(uint32_t bucketUs = 1000, uint32_t maxUs = 200000);

    void record(uint64_t microseconds);

    [[nodiscard]] uint64_t count() const noexcept { return count_; }
    [[nodiscard]] uint64_t min() const noexcept { return count_ ? min_ : 0; }
    [[nodiscard]] uint64_t max() const noexcept { return max_; }
    [[nodiscard]] double mean() const noexcept {
        return count_ ? static_cast<double>(sum_) / count_ : 0.0;
    }

    // Approximate percentile in microseconds (p in [0,1]); returns the midpoint
    // of the bucket where the cumulative count crosses p. 0 if no samples.
    [[nodiscard]] double percentile(double p) const;

    void reset();

private:
    uint32_t bucketUs_;
    uint32_t maxUs_;
    std::vector<uint64_t> buckets_; // last element is the overflow bucket
    uint64_t count_ = 0;
    uint64_t sum_ = 0;
    uint64_t min_ = 0;
    uint64_t max_ = 0;
};

// RFC 3550-style packet-loss estimator from RTP sequence numbers. Tracks the
// extended (wrap-aware) highest sequence number seen and how many packets were
// actually received, so loss% = 1 - received/expected.
class LossEstimator {
public:
    // Feed each received packet's 16-bit RTP sequence number, in arrival order.
    void observe(uint16_t sequenceNumber);

    [[nodiscard]] uint64_t received() const noexcept { return received_; }
    // Total packets that *should* have arrived: highest extended seq - base + 1.
    [[nodiscard]] uint64_t expected() const noexcept;
    [[nodiscard]] uint64_t lost() const noexcept;
    [[nodiscard]] double lossPercent() const;

    void reset();

private:
    bool init_ = false;
    uint16_t baseSeq_ = 0;
    uint16_t maxSeq_ = 0;
    uint64_t cycles_ = 0;     // number of 16-bit wraps observed
    uint64_t received_ = 0;
};

// Process-wide CPU utilisation sampler. utilizationPercent() returns CPU-seconds
// consumed per wall-second since the last call, as a percentage (100% == one
// fully-busy core; can exceed 100% on multiple cores). Use it to attribute load
// while running N media streams to derive "cost per stream / per core".
class CpuSampler {
public:
    CpuSampler();
    // CPU% since construction or the previous call to this method.
    [[nodiscard]] double utilizationPercent();

private:
    uint64_t lastCpuNs_;
    uint64_t lastWallNs_;
    static uint64_t processCpuNanos();
    static uint64_t wallNanos();
};

} // namespace vc::metrics
