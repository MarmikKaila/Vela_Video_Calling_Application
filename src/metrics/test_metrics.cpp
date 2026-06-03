// Metrics tests: histogram percentiles, packet-loss estimation (incl. wrap),
// and a smoke test of the CPU sampler.

#include <gtest/gtest.h>

#include "metrics/Metrics.h"

using namespace vc::metrics;

TEST(LatencyHistogram, PercentilesOverUniformSamples) {
    LatencyHistogram h(/*bucketUs=*/10, /*maxUs=*/100000);
    for (uint64_t us = 0; us < 1000; ++us) h.record(us); // 0..999us uniform

    EXPECT_EQ(h.count(), 1000u);
    EXPECT_EQ(h.min(), 0u);
    EXPECT_EQ(h.max(), 999u);
    EXPECT_NEAR(h.mean(), 499.5, 1.0);
    // p50 ~ 500us, p90 ~ 900us, within one bucket (10us).
    EXPECT_NEAR(h.percentile(0.50), 500.0, 15.0);
    EXPECT_NEAR(h.percentile(0.90), 900.0, 15.0);
}

TEST(LatencyHistogram, OverflowBucket) {
    LatencyHistogram h(1000, 50000); // max 50ms
    h.record(10'000);
    h.record(999'999); // way over max -> overflow bucket
    EXPECT_EQ(h.max(), 999'999u);
    EXPECT_LE(h.percentile(0.99), 50'000.0); // overflow reported at max edge
}

TEST(LossEstimator, CountsMissingPackets) {
    LossEstimator e;
    for (uint16_t s = 0; s < 100; ++s) {
        if (s == 50 || s == 51) continue; // drop two
        e.observe(s);
    }
    EXPECT_EQ(e.received(), 98u);
    EXPECT_EQ(e.expected(), 100u); // 0..99
    EXPECT_EQ(e.lost(), 2u);
    EXPECT_NEAR(e.lossPercent(), 2.0, 1e-9);
}

TEST(LossEstimator, HandlesSequenceWraparound) {
    LossEstimator e;
    // 65534, 65535, 0, 1, 2 — contiguous across the wrap, no loss.
    for (uint16_t s : {uint16_t(65534), uint16_t(65535), uint16_t(0), uint16_t(1), uint16_t(2)}) {
        e.observe(s);
    }
    EXPECT_EQ(e.received(), 5u);
    EXPECT_EQ(e.expected(), 5u);
    EXPECT_EQ(e.lost(), 0u);
}

TEST(LossEstimator, OutOfOrderDoesNotInflateExpected) {
    LossEstimator e;
    e.observe(0);
    e.observe(2);
    e.observe(1); // late, in-window
    EXPECT_EQ(e.received(), 3u);
    EXPECT_EQ(e.expected(), 3u); // 0..2
    EXPECT_EQ(e.lost(), 0u);
}

TEST(CpuSampler, ReturnsNonNegativeUtilization) {
    CpuSampler s;
    volatile double acc = 0;
    for (int i = 0; i < 1'000'000; ++i) acc += i * 0.5; // burn a little CPU
    (void)acc;
    const double util = s.utilizationPercent();
    EXPECT_GE(util, 0.0);
}
