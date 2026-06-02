#pragma once

// PixelConvert — small, dependency-free pixel format converters used by the
// macOS capture backend to normalize device output to canonical I420.
//
// These are deliberately plain C++ (no AVFoundation, no Accelerate) so they are
// unit-testable in isolation on any platform and reusable from the .mm backend.
// All functions write a tightly-packed I420 layout (Y plane, then U, then V)
// matching VideoFrame::makeI420 / VideoFrame::i420Size, and tolerate source
// strides larger than the visible width (capture buffers are often padded).
//
// Chroma is 4:2:0 with the standard (w+1)/2 x (h+1)/2 subsampling.

#include <cstdint>
#include <cstddef>

namespace vc::pixel {

// Plane geometry helpers (mirror VideoFrame's I420 layout).
[[nodiscard]] inline int chromaWidth(int w) noexcept { return (w + 1) / 2; }
[[nodiscard]] inline int chromaHeight(int h) noexcept { return (h + 1) / 2; }

// NV12 (Y plane + interleaved UV plane) -> I420.
//   srcY      : luma plane, srcYStride bytes per row
//   srcUV     : interleaved chroma plane (U0 V0 U1 V1 ...), srcUVStride b/row
//   dst       : destination I420 buffer, at least i420Size(w,h) bytes
// Y is copied row-by-row; UV is de-interleaved into separate U and V planes.
inline void nv12ToI420(const uint8_t* srcY, int srcYStride,
                       const uint8_t* srcUV, int srcUVStride,
                       int w, int h, uint8_t* dst) noexcept {
    const int cw = chromaWidth(w);
    const int ch = chromaHeight(h);
    uint8_t* dstY = dst;
    uint8_t* dstU = dst + static_cast<std::size_t>(w) * h;
    uint8_t* dstV = dstU + static_cast<std::size_t>(cw) * ch;

    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = srcY + static_cast<std::size_t>(y) * srcYStride;
        uint8_t* drow = dstY + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) drow[x] = srow[x];
    }
    for (int y = 0; y < ch; ++y) {
        const uint8_t* srow = srcUV + static_cast<std::size_t>(y) * srcUVStride;
        uint8_t* du = dstU + static_cast<std::size_t>(y) * cw;
        uint8_t* dv = dstV + static_cast<std::size_t>(y) * cw;
        for (int x = 0; x < cw; ++x) {
            du[x] = srow[2 * x + 0];
            dv[x] = srow[2 * x + 1];
        }
    }
}

// BGRA (8-bit packed, B G R A byte order) -> I420 using BT.601 full-range-ish
// coefficients. Chroma is computed from the top-left pixel of each 2x2 block
// (box-sampling would be marginally higher quality; point sampling keeps the
// hot path cheap and is visually fine for camera preview/encode input).
inline uint8_t clamp8(int v) noexcept {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline void bgraToI420(const uint8_t* src, int srcStride,
                      int w, int h, uint8_t* dst) noexcept {
    const int cw = chromaWidth(w);
    const int ch = chromaHeight(h);
    uint8_t* dstY = dst;
    uint8_t* dstU = dst + static_cast<std::size_t>(w) * h;
    uint8_t* dstV = dstU + static_cast<std::size_t>(cw) * ch;

    for (int y = 0; y < h; ++y) {
        const uint8_t* srow = src + static_cast<std::size_t>(y) * srcStride;
        uint8_t* drow = dstY + static_cast<std::size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            const uint8_t b = srow[4 * x + 0];
            const uint8_t g = srow[4 * x + 1];
            const uint8_t r = srow[4 * x + 2];
            // BT.601: Y = 0.257R + 0.504G + 0.098B + 16
            drow[x] = clamp8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (int cy = 0; cy < ch; ++cy) {
        const int sy = cy * 2;
        const uint8_t* srow = src + static_cast<std::size_t>(sy) * srcStride;
        uint8_t* du = dstU + static_cast<std::size_t>(cy) * cw;
        uint8_t* dv = dstV + static_cast<std::size_t>(cy) * cw;
        for (int cx = 0; cx < cw; ++cx) {
            const int sx = cx * 2;
            const uint8_t b = srow[4 * sx + 0];
            const uint8_t g = srow[4 * sx + 1];
            const uint8_t r = srow[4 * sx + 2];
            // U = -0.148R - 0.291G + 0.439B + 128
            du[cx] = clamp8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            // V =  0.439R - 0.368G - 0.071B + 128
            dv[cx] = clamp8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

} // namespace vc::pixel
