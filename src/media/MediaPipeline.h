#pragma once

// MediaPipeline — the wiring that turns the standalone capture/codec/network
// modules into an end-to-end media path. Split into two composable, fully
// dependency-injected halves so each is testable without a camera or a socket:
//
//   SendPipeline:    VideoFrame/AudioFrame --encode--> EncodedFrame
//                    --packetize--> RtpPacket(s) --> PacketSink (transport out)
//
//   ReceivePipeline: RtpPacket --> JitterBuffer --pop--> EncodedFrame
//                    --decode--> VideoFrame/AudioFrame --> frame sinks (render)
//
// Encoders/decoders are injected by pointer (the caller owns and opens them),
// so a test can drive real codecs over synthetic frames with no hardware.
//
// Threading:
//   * SendPipeline::push*() runs synchronously on the caller's thread (e.g. the
//     capture thread) — encode, packetize, emit, return. The PacketSink may be
//     invoked from that same thread.
//   * ReceivePipeline::pushPacket() runs on the receive/transport thread and only
//     touches the (thread-safe) JitterBuffer. tick() runs on the render thread
//     and is the ONLY caller of the decoders and the frame sinks. Do not call
//     tick() from two threads concurrently.

#include <cstdint>
#include <functional>
#include <optional>

#include "codec/AudioDecoder.h"
#include "codec/AudioEncoder.h"
#include "codec/VideoDecoder.h"
#include "codec/VideoEncoder.h"
#include "common/AudioFrame.h"
#include "common/EncodedFrame.h"
#include "common/Error.h"
#include "common/VideoFrame.h"
#include "network/JitterBuffer.h"
#include "network/RTPHandler.h"
#include "network/RtpPacket.h"

namespace vc {

// ---- SendPipeline -----------------------------------------------------------

class SendPipeline {
public:
    using PacketSink = std::function<void(const RtpPacket&)>;
    // Optional tap on the encoded frame before packetization (metrics, keyframe
    // tracking). Invoked synchronously inside push*().
    using EncodedTap = std::function<void(const EncodedFrame&)>;

    struct Config {
        uint8_t videoPayloadType = 96;  // dynamic PT for H.264
        uint32_t videoSsrc = 0;
        uint8_t audioPayloadType = 111; // dynamic PT for Opus
        uint32_t audioSsrc = 0;
    };

    // `videoEncoder` must be non-null and already open(); `audioEncoder` may be
    // null for a video-only pipeline. `sink` receives every produced RtpPacket.
    SendPipeline(VideoEncoder* videoEncoder, AudioEncoder* audioEncoder,
                 PacketSink sink, Config config);

    // Encode + packetize + emit one captured video frame. Returns ok even when
    // the encoder produced no output yet (it just emits nothing); returns the
    // encoder/packetizer error otherwise.
    [[nodiscard]] Status pushVideoFrame(const VideoFrame& frame);

    // As above for audio. Fails NotInitialized if no audio encoder was provided.
    [[nodiscard]] Status pushAudioFrame(const AudioFrame& frame);

    // Ask the video encoder to emit an IDR on its next frame (SFU join / PLI).
    void forceKeyframe();
    // Update the video encoder's target bitrate (REMB-driven ABR).
    void setVideoBitrate(int kbps);

    void setEncodedTap(EncodedTap tap) { encodedTap_ = std::move(tap); }

private:
    VideoEncoder* videoEncoder_;
    AudioEncoder* audioEncoder_;
    PacketSink sink_;
    Config config_;
    EncodedTap encodedTap_;

    RTPHandler videoRtp_;
    std::optional<RTPHandler> audioRtp_;
};

// ---- ReceivePipeline --------------------------------------------------------

class ReceivePipeline {
public:
    using VideoSink = std::function<void(const VideoFrame&)>;
    using AudioSink = std::function<void(const AudioFrame&)>;

    struct Config {
        uint32_t videoSsrc = 0;
        uint32_t audioSsrc = 0;                          // 0 => no audio
        std::chrono::milliseconds targetDelay{50};
    };

    // Decoders are injected (caller owns/opens). `audioDecoder` may be null.
    ReceivePipeline(VideoDecoder* videoDecoder, AudioDecoder* audioDecoder,
                    Config config, VideoSink videoSink, AudioSink audioSink = {});

    // Feed one received packet (receive thread). Routed to the audio or video
    // jitter buffer by SSRC; unknown SSRCs default to the video buffer.
    void pushPacket(const RtpPacket& pkt, SteadyTime arrival = MediaClock::now());

    // Drain whatever is ready for playout at `now`: pop complete frames, decode,
    // and deliver to the sinks (render thread only).
    void tick(SteadyTime now = MediaClock::now());

private:
    VideoDecoder* videoDecoder_;
    AudioDecoder* audioDecoder_;
    Config config_;
    VideoSink videoSink_;
    AudioSink audioSink_;

    JitterBuffer videoJitter_;
    std::optional<JitterBuffer> audioJitter_;
};

// ---- Capture glue (kept out of the core so the pipeline needs no camera) -----

// Connect a capture device's frame callback to a SendPipeline. The returned
// callback is what you pass to CaptureDevice::start(); it forwards each captured
// frame into the pipeline. Declared here, defined in MediaPipeline.cpp.
[[nodiscard]] std::function<void(const VideoFrame&)> makeCaptureSink(SendPipeline& pipeline);

} // namespace vc
