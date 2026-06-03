#include "metrics/Metrics.h"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace vc::metrics {

// ---- LatencyHistogram -------------------------------------------------------

LatencyHistogram::LatencyHistogram(uint32_t bucketUs, uint32_t maxUs)
    : bucketUs_(bucketUs == 0 ? 1 : bucketUs), maxUs_(maxUs) {
    const std::size_t n = maxUs_ / bucketUs_ + 1; // +1 overflow bucket
    buckets_.assign(n, 0);
}

void LatencyHistogram::record(uint64_t microseconds) {
    std::size_t idx = microseconds / bucketUs_;
    if (idx >= buckets_.size()) idx = buckets_.size() - 1; // overflow
    ++buckets_[idx];

    if (count_ == 0 || microseconds < min_) min_ = microseconds;
    if (microseconds > max_) max_ = microseconds;
    sum_ += microseconds;
    ++count_;
}

double LatencyHistogram::percentile(double p) const {
    if (count_ == 0) return 0.0;
    p = std::clamp(p, 0.0, 1.0);
    const uint64_t target = static_cast<uint64_t>(p * static_cast<double>(count_));
    uint64_t cumulative = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
        cumulative += buckets_[i];
        if (cumulative >= target && buckets_[i] > 0) {
            // Midpoint of bucket i, or the bucket lower edge for the overflow.
            if (i == buckets_.size() - 1) return static_cast<double>(maxUs_);
            return (static_cast<double>(i) + 0.5) * bucketUs_;
        }
    }
    return static_cast<double>(max_);
}

void LatencyHistogram::reset() {
    std::fill(buckets_.begin(), buckets_.end(), 0);
    count_ = sum_ = min_ = max_ = 0;
}

// ---- LossEstimator ----------------------------------------------------------

void LossEstimator::observe(uint16_t seq) {
    if (!init_) {
        init_ = true;
        baseSeq_ = seq;
        maxSeq_ = seq;
        received_ = 1;
        return;
    }
    ++received_;
    // Detect a forward 16-bit wrap: a large negative step means seq advanced
    // past 65535 back to a small value.
    const uint16_t delta = static_cast<uint16_t>(seq - maxSeq_);
    if (delta != 0 && delta < 0x8000) {
        // seq is "ahead" of maxSeq_ in RFC 1982 terms.
        if (seq < maxSeq_) ++cycles_; // crossed the wrap boundary
        maxSeq_ = seq;
    }
    // Out-of-order / late packets (delta >= 0x8000) don't move the high-water
    // mark; they still count as received.
}

uint64_t LossEstimator::expected() const noexcept {
    if (!init_) return 0;
    const uint64_t extendedMax = cycles_ * 65536ull + maxSeq_;
    return extendedMax - baseSeq_ + 1;
}

uint64_t LossEstimator::lost() const noexcept {
    const uint64_t exp = expected();
    return exp > received_ ? exp - received_ : 0;
}

double LossEstimator::lossPercent() const {
    const uint64_t exp = expected();
    return exp == 0 ? 0.0 : 100.0 * static_cast<double>(lost()) / static_cast<double>(exp);
}

void LossEstimator::reset() { *this = LossEstimator{}; }

// ---- CpuSampler -------------------------------------------------------------

uint64_t CpuSampler::processCpuNanos() {
    // CLOCK_PROCESS_CPUTIME_ID where available (Linux/macOS); falls back to clock().
#if defined(CLOCK_PROCESS_CPUTIME_ID)
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0) {
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull +
               static_cast<uint64_t>(ts.tv_nsec);
    }
#endif
    return static_cast<uint64_t>(std::clock()) * (1'000'000'000ull / CLOCKS_PER_SEC);
}

uint64_t CpuSampler::wallNanos() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

CpuSampler::CpuSampler() : lastCpuNs_(processCpuNanos()), lastWallNs_(wallNanos()) {}

double CpuSampler::utilizationPercent() {
    const uint64_t cpu = processCpuNanos();
    const uint64_t wall = wallNanos();
    const uint64_t dCpu = cpu - lastCpuNs_;
    const uint64_t dWall = wall - lastWallNs_;
    lastCpuNs_ = cpu;
    lastWallNs_ = wall;
    if (dWall == 0) return 0.0;
    return 100.0 * static_cast<double>(dCpu) / static_cast<double>(dWall);
}

} // namespace vc::metrics
