// test_capture — unit tests for the capture module's format conversion.
//
// Exercises NV12->I420 and BGRA->I420 on synthetic buffers (no camera needed),
// verifying plane sizes, de-interleaving, padded-stride handling and luma math.

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "capture/PixelConvert.h"
#include "common/VideoFrame.h"

using namespace vc;

TEST(PixelConvert, Nv12ToI420DeinterleavesChroma) {
    constexpr int w = 4, h = 4;
    const int cw = pixel::chromaWidth(w);   // 2
    const int ch = pixel::chromaHeight(h);  // 2

    // Y plane: value == row*w + col, packed (stride == w).
    std::vector<uint8_t> y(static_cast<std::size_t>(w) * h);
    for (int r = 0; r < h; ++r)
        for (int c = 0; c < w; ++c) y[r * w + c] = static_cast<uint8_t>(r * w + c);

    // Interleaved UV plane: U=10+block, V=200+block (distinct ranges).
    std::vector<uint8_t> uv(static_cast<std::size_t>(cw) * ch * 2);
    for (int r = 0; r < ch; ++r) {
        for (int c = 0; c < cw; ++c) {
            const int blk = r * cw + c;
            uv[(r * cw + c) * 2 + 0] = static_cast<uint8_t>(10 + blk);
            uv[(r * cw + c) * 2 + 1] = static_cast<uint8_t>(200 + blk);
        }
    }

    std::vector<uint8_t> dst(VideoFrame::i420Size(w, h));
    pixel::nv12ToI420(y.data(), w, uv.data(), cw * 2, w, h, dst.data());

    const uint8_t* dY = dst.data();
    const uint8_t* dU = dY + static_cast<std::size_t>(w) * h;
    const uint8_t* dV = dU + static_cast<std::size_t>(cw) * ch;

    for (std::size_t i = 0; i < y.size(); ++i) EXPECT_EQ(dY[i], y[i]) << "Y[" << i << "]";
    for (int blk = 0; blk < cw * ch; ++blk) {
        EXPECT_EQ(dU[blk], static_cast<uint8_t>(10 + blk)) << "U block " << blk;
        EXPECT_EQ(dV[blk], static_cast<uint8_t>(200 + blk)) << "V block " << blk;
    }
}

TEST(PixelConvert, Nv12ToI420HandlesPaddedStride) {
    constexpr int w = 2, h = 2;
    const int cw = pixel::chromaWidth(w);   // 1
    const int ch = pixel::chromaHeight(h);  // 1
    const int yStride = 8;   // padded well beyond w
    const int uvStride = 8;

    std::vector<uint8_t> y(static_cast<std::size_t>(yStride) * h, 0xEE);
    y[0 * yStride + 0] = 1; y[0 * yStride + 1] = 2;
    y[1 * yStride + 0] = 3; y[1 * yStride + 1] = 4;

    std::vector<uint8_t> uv(static_cast<std::size_t>(uvStride) * ch, 0xEE);
    uv[0] = 50;  // U
    uv[1] = 60;  // V

    std::vector<uint8_t> dst(VideoFrame::i420Size(w, h));
    pixel::nv12ToI420(y.data(), yStride, uv.data(), uvStride, w, h, dst.data());

    const uint8_t* dY = dst.data();
    EXPECT_EQ(dY[0], 1); EXPECT_EQ(dY[1], 2);
    EXPECT_EQ(dY[2], 3); EXPECT_EQ(dY[3], 4); // packed, no padding bytes leaked
    const uint8_t* dU = dY + w * h;
    const uint8_t* dV = dU + cw * ch;
    EXPECT_EQ(dU[0], 50);
    EXPECT_EQ(dV[0], 60);
}

TEST(PixelConvert, BgraToI420LumaMatchesBt601) {
    constexpr int w = 2, h = 2;
    std::vector<uint8_t> src(static_cast<std::size_t>(w) * h * 4);
    auto setPix = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
        uint8_t* p = src.data() + (y * w + x) * 4;
        p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
    };
    // White, black, pure blue, pure red. setPix args are (b, g, r).
    setPix(0, 0, 255, 255, 255);
    setPix(1, 0, 0, 0, 0);
    setPix(0, 1, 255, 0, 0);  // blue at (0,1)
    setPix(1, 1, 0, 0, 255);  // red at (1,1)

    std::vector<uint8_t> dst(VideoFrame::i420Size(w, h));
    pixel::bgraToI420(src.data(), w * 4, w, h, dst.data());

    const uint8_t* dY = dst.data();
    // White -> ~235, black -> 16 (BT.601 studio range with +16 offset).
    EXPECT_NEAR(dY[0], 235, 2);
    EXPECT_EQ(dY[1], 16);
    // Red Y = (66*255+128>>8)+16 ~= 81; Blue Y ~= 41.
    EXPECT_NEAR(dY[w * 1 + 0], 41, 2);  // blue at (0,1)
    EXPECT_NEAR(dY[w * 1 + 1], 81, 2);  // red at (1,1)
}

TEST(PixelConvert, BgraToI420ChromaSizing) {
    constexpr int w = 6, h = 4;
    std::vector<uint8_t> src(static_cast<std::size_t>(w) * h * 4, 128);
    std::vector<uint8_t> dst(VideoFrame::i420Size(w, h), 0);
    pixel::bgraToI420(src.data(), w * 4, w, h, dst.data());
    // Gray input -> chroma should sit near neutral 128.
    const std::size_t ySize = static_cast<std::size_t>(w) * h;
    EXPECT_NEAR(dst[ySize], 128, 2);
    EXPECT_NEAR(dst.back(), 128, 2);
}
