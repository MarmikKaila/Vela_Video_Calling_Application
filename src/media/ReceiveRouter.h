#pragma once

// ReceiveRouter — fan one inbound RTP stream out into per-sender media streams.
//
// In the SFU topology a client receives, over a single ICE channel, the RTP of
// every other participant interleaved together. Each (sender, media-kind) pair
// has its own SSRC. ReceiveRouter demultiplexes by SSRC: it keeps a JitterBuffer
// + decoder per stream, distinguishing video from audio by RTP payload type
// (96 = H.264, 111 = Opus). Streams appear and disappear as people join/leave.
//
// pushPacket() runs on the transport (libjuice) thread and only touches the
// thread-safe JitterBuffers — it never creates Qt objects or decodes. tick()
// runs on the GUI/render thread and is the ONLY place decoders run and the
// stream-add/-remove and frame callbacks fire, so the caller can safely create
// or destroy UI tiles there. A stream that receives no packet for `streamTimeout`
// is reaped (the sender left), firing onVideoStreamRemoved.

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "audio/AudioIO.h"
#include "codec/AudioDecoder.h"
#include "codec/VideoDecoder.h"
#include "common/Clock.h"
#include "common/VideoFrame.h"
#include "network/JitterBuffer.h"
#include "network/RtpPacket.h"

namespace vc {

class ReceiveRouter {
public:
    struct Config {
        uint8_t videoPayloadType = 96;
        uint8_t audioPayloadType = 111;
        std::chrono::milliseconds targetDelay{50};
        std::chrono::milliseconds streamTimeout{3000};  // reap idle streams
    };

    // Callbacks all fire on the thread that calls tick() (the GUI thread):
    //   onVideoFrame(ssrc, frame)      — a decoded picture for that sender
    //   onVideoStreamAdded(ssrc)       — first time a video SSRC is seen
    //   onVideoStreamRemoved(ssrc)     — that video sender went idle/left
    // Audio is played directly through `playback` (may be null for video-only).
    using VideoFrameFn = std::function<void(uint32_t ssrc, const VideoFrame&)>;
    using StreamFn = std::function<void(uint32_t ssrc)>;

    ReceiveRouter(Config cfg, audio::AudioPlayback* playback, VideoFrameFn onVideoFrame,
                  StreamFn onVideoStreamAdded, StreamFn onVideoStreamRemoved);

    // Feed one inbound RTP packet (transport thread). Lazily creates the stream.
    void pushPacket(const RtpPacket& pkt, SteadyTime arrival = MediaClock::now());

    // Drain ready frames, decode, and deliver; reap idle streams (GUI thread).
    void tick(SteadyTime now = MediaClock::now());

private:
    struct Stream {
        bool video = false;
        std::unique_ptr<JitterBuffer> jitter;
        std::unique_ptr<VideoDecoder> vdec;  // video only
        std::unique_ptr<AudioDecoder> adec;  // audio only
        bool announced = false;              // onVideoStreamAdded fired
        SteadyTime lastPacket{};
    };

    Config cfg_;
    audio::AudioPlayback* playback_;
    VideoFrameFn onVideoFrame_;
    StreamFn onVideoStreamAdded_;
    StreamFn onVideoStreamRemoved_;

    std::mutex mu_;
    std::map<uint32_t, std::unique_ptr<Stream>> streams_;  // keyed by SSRC
};

}  // namespace vc
