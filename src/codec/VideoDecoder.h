#pragma once

// VideoDecoder — H.264 -> I420 VideoFrame, wrapping FFmpeg libavcodec.
//
// Consumes EncodedFrames carrying Annex-B H.264 (as produced by VideoEncoder or
// arriving from the network after RTP de-packetization) and produces raw I420
// VideoFrames. Decoded pixels are copied into a buffer drawn from a FramePool
// the decoder owns, so the steady-state path performs no per-frame heap
// allocation and downstream consumers share the frame by reference-count.
//
// The pool is sized lazily on the first decoded frame once the true output
// resolution is known (the stream may differ from any hint), and re-sized if a
// later frame is larger. rtpTimestamp and captureTime are carried through from
// the input EncodedFrame.
//
// Lifetime: open() once before decode(). The AVCodecContext is owned via a
// unique_ptr with a custom deleter (RAII). Not thread-safe.

#include <cstdint>
#include <memory>

#include "common/EncodedFrame.h"
#include "common/Error.h"
#include "common/FramePool.h"
#include "common/Result.h"
#include "common/VideoFrame.h"

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace vc {

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;
    VideoDecoder(VideoDecoder&&) noexcept = default;
    VideoDecoder& operator=(VideoDecoder&&) noexcept = default;

    // Allocate and open the H.264 decoder context. Safe to re-call (resets).
    [[nodiscard]] Status open();

    // Decode one access unit into an I420 VideoFrame. Returns CodecError on a
    // hard decode failure, ResourceExhausted if the frame pool is momentarily
    // empty (caller should drop), or Internal on an unexpected output format.
    // Note: an empty Result-error may also mean the decoder needs more data
    // before it can emit a picture; this is reported as CodecError so callers
    // can simply feed the next packet.
    [[nodiscard]] Result<VideoFrame, Error> decode(const EncodedFrame& frame);

    [[nodiscard]] bool isOpen() const noexcept { return ctx_ != nullptr; }

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

    // Ensure the owned pool can supply I420 buffers for a w*h frame.
    void ensurePool(int w, int h);

    std::unique_ptr<AVCodecContext, AVCodecContextDeleter> ctx_;
    std::unique_ptr<AVFrame, AVFrameDeleter> frame_;    // reused output frame
    std::unique_ptr<AVPacket, AVPacketDeleter> packet_; // reused input packet

    std::shared_ptr<FramePool> pool_;
    int poolW_ = 0;
    int poolH_ = 0;
};

} // namespace vc
