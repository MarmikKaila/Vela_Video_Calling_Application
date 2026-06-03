#include "media/ReceiveRouter.h"

#include <utility>
#include <vector>

namespace vc {

namespace {
JitterBufferConfig makeJbConfig(bool video, std::chrono::milliseconds delay) {
    JitterBufferConfig c;
    c.targetDelay = delay;
    c.kind = video ? MediaKind::Video : MediaKind::Audio;
    c.videoCodec = VideoCodec::H264;
    c.audioCodec = AudioCodec::Opus;
    return c;
}
}  // namespace

ReceiveRouter::ReceiveRouter(Config cfg, audio::AudioPlayback* playback,
                             VideoFrameFn onVideoFrame, StreamFn onVideoStreamAdded,
                             StreamFn onVideoStreamRemoved)
    : cfg_(cfg),
      playback_(playback),
      onVideoFrame_(std::move(onVideoFrame)),
      onVideoStreamAdded_(std::move(onVideoStreamAdded)),
      onVideoStreamRemoved_(std::move(onVideoStreamRemoved)) {}

void ReceiveRouter::pushPacket(const RtpPacket& pkt, SteadyTime arrival) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = streams_.find(pkt.ssrc);
    if (it == streams_.end()) {
        // First sight of this SSRC: decide kind by payload type and build the
        // per-stream jitter buffer + decoder. Unknown PTs are treated as video.
        const bool isAudio = (pkt.payloadType == cfg_.audioPayloadType);
        auto s = std::make_unique<Stream>();
        s->video = !isAudio;
        s->jitter = std::make_unique<JitterBuffer>(makeJbConfig(s->video, cfg_.targetDelay));
        if (s->video) {
            s->vdec = std::make_unique<VideoDecoder>();
            (void)s->vdec->open();
        } else {
            s->adec = std::make_unique<AudioDecoder>();
            AudioFormat fmt;  // 48 kHz mono, matches the sender/playback
            fmt.sampleRateHz = 48000;
            fmt.channels = 1;
            (void)s->adec->open(fmt);
        }
        it = streams_.emplace(pkt.ssrc, std::move(s)).first;
    }
    it->second->lastPacket = arrival;
    it->second->jitter->push(pkt, arrival);
}

void ReceiveRouter::tick(SteadyTime now) {
    // Phase 1 (locked): pull ready EncodedFrames per stream, note which video
    // streams are newly announced, and collect idle streams to reap. Decoding
    // and callbacks happen after unlocking so the transport thread is not blocked
    // and the decoders (touched only here) need no locking.
    struct Ready {
        uint32_t ssrc;
        bool video;
        bool announce;
        VideoDecoder* vdec;
        AudioDecoder* adec;
        std::vector<EncodedFrame> frames;
    };
    std::vector<Ready> ready;
    std::vector<uint32_t> removedVideo;

    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = streams_.begin(); it != streams_.end();) {
            Stream& s = *it->second;
            if (now - s.lastPacket > cfg_.streamTimeout) {
                if (s.video && s.announced) removedVideo.push_back(it->first);
                it = streams_.erase(it);
                continue;
            }
            Ready r;
            r.ssrc = it->first;
            r.video = s.video;
            r.vdec = s.vdec.get();
            r.adec = s.adec.get();
            r.announce = false;
            while (auto ef = s.jitter->pop(now)) {
                if (s.video && !s.announced) {
                    s.announced = true;
                    r.announce = true;
                }
                r.frames.push_back(std::move(*ef));
            }
            if (!r.frames.empty()) ready.push_back(std::move(r));
            ++it;
        }
    }

    for (auto& r : ready) {
        if (r.video) {
            if (r.announce && onVideoStreamAdded_) onVideoStreamAdded_(r.ssrc);
            for (const auto& ef : r.frames) {
                auto decoded = r.vdec->decode(ef);
                if (decoded && onVideoFrame_) onVideoFrame_(r.ssrc, decoded.value());
            }
        } else {
            for (const auto& ef : r.frames) {
                auto decoded = r.adec->decode(ef);
                if (decoded && playback_) playback_->enqueue(decoded.value());
            }
        }
    }

    for (uint32_t ssrc : removedVideo) {
        if (onVideoStreamRemoved_) onVideoStreamRemoved_(ssrc);
    }
}

}  // namespace vc
