#pragma once

// Shared media value types. These describe *what* media looks like (formats,
// resolutions, codec identities) without pulling in any platform or library
// headers, so every module — capture, codec, network, SFU, UI — can depend on
// them without coupling to FFmpeg/Qt/etc.

#include <cstdint>
#include <string_view>

namespace vc {

// Raw pixel layouts a CaptureDevice may produce or an encoder may consume.
// I420 is the canonical interchange format inside the pipeline; capture
// backends that deliver something else convert to I420 before handing frames
// downstream (or advertise their native format for a zero-copy fast path).
enum class PixelFormat {
    Unknown = 0,
    I420,  // planar Y, U, V (4:2:0) — pipeline interchange format
    NV12,  // planar Y, interleaved UV (4:2:0) — common on macOS/Windows capture
    YUYV,  // packed 4:2:2 — common V4L2 webcam output
    RGBA,  // packed 8-bit, render/debug
    BGRA,  // packed 8-bit, common macOS/Windows
};

[[nodiscard]] constexpr int planeCount(PixelFormat f) noexcept {
    switch (f) {
        case PixelFormat::I420: return 3;
        case PixelFormat::NV12: return 2;
        case PixelFormat::YUYV:
        case PixelFormat::RGBA:
        case PixelFormat::BGRA: return 1;
        case PixelFormat::Unknown: return 0;
    }
    return 0;
}

struct VideoResolution {
    int width = 0;
    int height = 0;

    constexpr bool operator==(const VideoResolution& o) const noexcept {
        return width == o.width && height == o.height;
    }
    [[nodiscard]] constexpr bool valid() const noexcept {
        return width > 0 && height > 0;
    }
};

// A capture capability the device advertises (resolution + frame rate + format).
struct VideoFormat {
    VideoResolution resolution{};
    int fpsNumerator = 0;
    int fpsDenominator = 1;
    PixelFormat pixelFormat = PixelFormat::Unknown;

    [[nodiscard]] constexpr double fps() const noexcept {
        return fpsDenominator ? static_cast<double>(fpsNumerator) / fpsDenominator : 0.0;
    }
};

// Audio is always interleaved 16-bit PCM inside the pipeline pre-encode.
struct AudioFormat {
    int sampleRateHz = 48000; // Opus operates at 48 kHz
    int channels = 1;
};

enum class MediaKind { Video, Audio };

enum class VideoCodec { H264, Unknown };
enum class AudioCodec { Opus, Unknown };

// RTP clock rates are codec-defined constants used everywhere timestamps are
// computed (RTPHandler, JitterBuffer, AVSync), so they live with the types.
inline constexpr uint32_t kVideoRtpClockHz = 90000;  // H.264 / all video in RTP
inline constexpr uint32_t kAudioRtpClockHz = 48000;  // Opus

} // namespace vc
