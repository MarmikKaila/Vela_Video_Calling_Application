// Unit test for the BT.601 limited-range YUV->RGB conversion the VideoWidget
// fragment shader performs. The shader math is replicated here on the CPU (it
// cannot be invoked directly without a GL context) and checked against known
// reference colors so the conversion constants stay correct.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace {

struct Rgb {
    float r, g, b;
};

// Mirror of the GLSL in VideoWidget.cpp. Inputs are 8-bit YUV; outputs are
// clamped [0,1] floats.
Rgb yuvToRgb(uint8_t y8, uint8_t u8, uint8_t v8) {
    const float y = 1.1643f * (y8 / 255.0f - 0.0625f);
    const float u = u8 / 255.0f - 0.5f;
    const float v = v8 / 255.0f - 0.5f;
    auto clamp01 = [](float x) { return std::clamp(x, 0.0f, 1.0f); };
    return {clamp01(y + 1.5958f * v),
            clamp01(y - 0.39173f * u - 0.81290f * v),
            clamp01(y + 2.017f * u)};
}

constexpr float kTol = 0.04f; // ~10/255

} // namespace

TEST(YuvToRgb, BlackMapsToBlack) {
    const Rgb c = yuvToRgb(16, 128, 128);
    EXPECT_NEAR(c.r, 0.0f, kTol);
    EXPECT_NEAR(c.g, 0.0f, kTol);
    EXPECT_NEAR(c.b, 0.0f, kTol);
}

TEST(YuvToRgb, WhiteMapsToWhite) {
    const Rgb c = yuvToRgb(235, 128, 128);
    EXPECT_NEAR(c.r, 1.0f, kTol);
    EXPECT_NEAR(c.g, 1.0f, kTol);
    EXPECT_NEAR(c.b, 1.0f, kTol);
}

TEST(YuvToRgb, PrimaryRed) {
    // BT.601 limited-range red bar.
    const Rgb c = yuvToRgb(81, 90, 240);
    EXPECT_NEAR(c.r, 1.0f, kTol);
    EXPECT_NEAR(c.g, 0.0f, kTol);
    EXPECT_NEAR(c.b, 0.0f, kTol);
}

TEST(YuvToRgb, PrimaryGreen) {
    const Rgb c = yuvToRgb(145, 54, 34);
    EXPECT_NEAR(c.r, 0.0f, kTol);
    EXPECT_NEAR(c.g, 1.0f, kTol);
    EXPECT_NEAR(c.b, 0.0f, kTol);
}

TEST(YuvToRgb, PrimaryBlue) {
    const Rgb c = yuvToRgb(41, 240, 110);
    EXPECT_NEAR(c.r, 0.0f, kTol);
    EXPECT_NEAR(c.g, 0.0f, kTol);
    EXPECT_NEAR(c.b, 1.0f, kTol);
}
