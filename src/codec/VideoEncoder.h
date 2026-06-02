#pragma once

// VideoEncoder — H.264 encoder wrapping FFmpeg libavcodec (libx264).
//
// Consumes raw I420 VideoFrames from the capture/conversion stage and produces
// EncodedFrames carrying an Annex-B H.264 byte stream (start-code prefixed NAL
// units), exactly one access unit per encode() call. The encoder is configured
// for real-time conferencing: zero-latency tuning (no frame reordering, no
// look-ahead) so encode() returns the packet for the frame just submitted with
// no buffered delay, plus a bounded GOP so periodic IDRs bound error
// propagation after loss.
//
// rtpTimestamp and captureTime are carried straight through from the input
// VideoFrame onto the output EncodedFrame — the encoder never invents timing.
//
// Lifetime: open(config) must be called once before encode(). The underlying
// AVCodecContext is owned through a unique_ptr with a custom deleter (RAII);
// destroying the VideoEncoder (or re-calling open()) frees it. Not thread-safe;
// drive a single encoder from one thread.

#include <cstdint>
#include <memory>

#include "common/EncodedFrame.h"
#include "common/Error.h"
#include "common/Result.h"
#include "common/VideoFrame.h"

// Forward-declare the FFmpeg types so callers don't pull in libavcodec headers.
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace vc {

class VideoEncoder {
public:
    // Encoder parameters. fps drives the codec time base and rate control; the
    // GOP (keyframe interval) is derived from it. bitrateKbps is the target
    // average bitrate for CBR-ish rate control and may be retuned at runtime via
    // setBitrate().
    struct Config {
        int width = 0;
        int height = 0;
        int fps = 30;
        int bitrateKbps = 2000;
    };

    VideoEncoder();
    ~VideoEncoder();

    VideoEncoder(const VideoEncoder&) = delete;
    VideoEncoder& operator=(const VideoEncoder&) = delete;
    VideoEncoder(VideoEncoder&&) noexcept = default;
    VideoEncoder& operator=(VideoEncoder&&) noexcept = default;

    // Allocate and open the libx264 context for the given configuration. Safe to
    // call again to reconfigure (the previous context is torn down first).
    [[nodiscard]] Status open(const Config& config);

    // Encode one I420 frame into a single Annex-B access unit. The keyframe flag
    // is set from AV_PKT_FLAG_KEY; rtpTimestamp/captureTime are copied from the
    // input frame. Returns CodecError on encoder failure, InvalidArgument if the
    // frame is not I420 / wrong size, NotInitialized if open() was not called.
    [[nodiscard]] Result<EncodedFrame, Error> encode(const VideoFrame& frame);

    // Request that the NEXT encode() emit an IDR (used to satisfy keyframe
    // requests when a participant joins or after packet loss).
    void forceKeyframe() noexcept { forceKeyframe_ = true; }

    // Retune the target bitrate at runtime (adaptive bitrate). No-op before
    // open(); takes effect from the next encoded frame.
    void setBitrate(int bitrateKbps);

    [[nodiscard]] bool isOpen() const noexcept { return ctx_ != nullptr; }
    [[nodiscard]] const Config& config() const noexcept { return config_; }

private:
    struct AVCodecContextDeleter {
        void operator()(AVCodecContext* c) const noexcept;
    };
    struct AVFrameDeleter {
        void operator()(AVFrame* f) const noexcept;
    };
    struct AVPacketDeleter {
        void operator()(AVPacket* p) const noexcept;
    };

    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> ctx_;
    std::unique_ptr<AVFrame, AVFrameDeleter> frame_;    // reused input wrapper
    std::unique_ptr<AVPacket, AVPacketDeleter> packet_; // reused output packet

    Config config_{};
    int64_t pts_ = 0;            // monotonically increasing codec PTS
    bool forceKeyframe_ = false; // request IDR on next encode
};

} // namespace vc
