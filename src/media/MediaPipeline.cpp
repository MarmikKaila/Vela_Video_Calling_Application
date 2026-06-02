#include "media/MediaPipeline.h"

#include <utility>

namespace vc {

// ---- SendPipeline -----------------------------------------------------------

SendPipeline::SendPipeline(VideoEncoder* videoEncoder, AudioEncoder* audioEncoder,
                           PacketSink sink, Config config)
    : videoEncoder_(videoEncoder),
      audioEncoder_(audioEncoder),
      sink_(std::move(sink)),
      config_(config),
      videoRtp_(config.videoPayloadType, config.videoSsrc, kVideoRtpClockHz) {
    if (audioEncoder_) {
        audioRtp_.emplace(config.audioPayloadType, config.audioSsrc, kAudioRtpClockHz);
    }
}

Status SendPipeline::pushVideoFrame(const VideoFrame& frame) {
    if (!videoEncoder_) return fail(Error::NotInitialized);
    auto encoded = videoEncoder_->encode(frame);
    if (!encoded) return fail(encoded.error());

    const EncodedFrame& ef = encoded.value();
    if (ef.empty()) return ok(); // encoder buffered; nothing to send this call
    if (encodedTap_) encodedTap_(ef);

    auto packets = videoRtp_.packetize(ef);
    if (!packets) return fail(packets.error());
    if (sink_) {
        for (const auto& pkt : packets.value()) sink_(pkt);
    }
    return ok();
}

Status SendPipeline::pushAudioFrame(const AudioFrame& frame) {
    if (!audioEncoder_ || !audioRtp_) return fail(Error::NotInitialized);
    auto encoded = audioEncoder_->encode(frame);
    if (!encoded) return fail(encoded.error());

    const EncodedFrame& ef = encoded.value();
    if (ef.empty()) return ok();
    if (encodedTap_) encodedTap_(ef);

    auto packets = audioRtp_->packetize(ef);
    if (!packets) return fail(packets.error());
    if (sink_) {
        for (const auto& pkt : packets.value()) sink_(pkt);
    }
    return ok();
}

void SendPipeline::forceKeyframe() {
    if (videoEncoder_) videoEncoder_->forceKeyframe();
}

void SendPipeline::setVideoBitrate(int kbps) {
    if (videoEncoder_) videoEncoder_->setBitrate(kbps);
}

// ---- ReceivePipeline --------------------------------------------------------

namespace {
JitterBufferConfig videoJbConfig(std::chrono::milliseconds delay) {
    JitterBufferConfig c;
    c.targetDelay = delay;
    c.kind = MediaKind::Video;
    c.videoCodec = VideoCodec::H264;
    return c;
}
JitterBufferConfig audioJbConfig(std::chrono::milliseconds delay) {
    JitterBufferConfig c;
    c.targetDelay = delay;
    c.kind = MediaKind::Audio;
    c.audioCodec = AudioCodec::Opus;
    return c;
}
} // namespace

ReceivePipeline::ReceivePipeline(VideoDecoder* videoDecoder, AudioDecoder* audioDecoder,
                                 Config config, VideoSink videoSink, AudioSink audioSink)
    : videoDecoder_(videoDecoder),
      audioDecoder_(audioDecoder),
      config_(config),
      videoSink_(std::move(videoSink)),
      audioSink_(std::move(audioSink)),
      videoJitter_(videoJbConfig(config.targetDelay)) {
    if (audioDecoder_ && config.audioSsrc != 0) {
        audioJitter_.emplace(audioJbConfig(config.targetDelay));
    }
}

void ReceivePipeline::pushPacket(const RtpPacket& pkt, SteadyTime arrival) {
    if (audioJitter_ && pkt.ssrc == config_.audioSsrc) {
        audioJitter_->push(pkt, arrival);
    } else {
        videoJitter_.push(pkt, arrival);
    }
}

void ReceivePipeline::tick(SteadyTime now) {
    // Drain all video frames whose target delay has elapsed.
    if (videoDecoder_) {
        while (auto ef = videoJitter_.pop(now)) {
            auto decoded = videoDecoder_->decode(*ef);
            if (decoded && videoSink_) videoSink_(decoded.value());
        }
    }
    // Drain audio similarly.
    if (audioJitter_ && audioDecoder_) {
        while (auto ef = audioJitter_->pop(now)) {
            auto decoded = audioDecoder_->decode(*ef);
            if (decoded && audioSink_) audioSink_(decoded.value());
        }
    }
}

// ---- Capture glue -----------------------------------------------------------

std::function<void(const VideoFrame&)> makeCaptureSink(SendPipeline& pipeline) {
    return [&pipeline](const VideoFrame& frame) {
        // Errors on the capture thread are swallowed here; the app can wrap this
        // sink to surface them. Dropping a frame under encoder error is the
        // correct real-time behavior (don't block the capture thread).
        (void)pipeline.pushVideoFrame(frame);
    };
}

} // namespace vc
