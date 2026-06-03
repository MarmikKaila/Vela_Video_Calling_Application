// MacAudio.mm — AVAudioEngine implementation of AudioCapture / AudioPlayback.
//
// Capture: an AVAudioEngine input-node tap delivers buffers at the hardware
// format; an AVAudioConverter resamples/reformats them to 48 kHz interleaved
// int16, which we re-chunk into exact 20 ms (960 samples/channel) AudioFrames.
//
// Playback: an AVAudioSourceNode pulls 48 kHz float from a mutex-guarded ring
// buffer that enqueue() fills with decoded int16 PCM (converted to float). The
// engine handles 48 kHz -> hardware-rate conversion. Underruns play silence.

#include "audio/AudioIO.h"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "common/Clock.h"
#include "common/FramePool.h"
#include "common/Types.h"

namespace vc::audio {

namespace {
constexpr int kRtpClockHz = 48000;       // Opus / audio RTP clock
constexpr int kFrameMs = 20;             // Opus frame duration
constexpr std::size_t kPoolDepth = 16;   // 20 ms frames in flight

int samplesPerFrame(int sampleRateHz) { return sampleRateHz * kFrameMs / 1000; }
}  // namespace

// ===========================================================================
// Capture
// ===========================================================================

class MacAudioCapture final : public AudioCapture {
public:
    ~MacAudioCapture() override { (void)stop(); }

    Status start(AudioFormat format, FrameCallback cb) override {
        if (capturing_) return fail(Error::AlreadyRunning);
        if (format.sampleRateHz != 48000 || format.channels < 1 || format.channels > 2)
            return fail(Error::InvalidArgument);
        if (!cb) return fail(Error::InvalidArgument);

        format_ = format;
        cb_ = std::move(cb);
        const int spf = samplesPerFrame(format_.sampleRateHz);
        const std::size_t frameBytes =
            static_cast<std::size_t>(spf) * format_.channels * sizeof(int16_t);
        pool_ = FramePool::create(frameBytes, kPoolDepth);
        accum_.clear();
        samplesEmitted_ = 0;

        @autoreleasepool {
            engine_ = [[AVAudioEngine alloc] init];
            AVAudioInputNode* input = engine_.inputNode;
            AVAudioFormat* inFmt = [input inputFormatForBus:0];
            if (inFmt.sampleRate <= 0) return fail(Error::DeviceError);

            outFmt_ = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatInt16
                          sampleRate:format_.sampleRateHz
                            channels:static_cast<AVAudioChannelCount>(format_.channels)
                         interleaved:YES];
            converter_ = [[AVAudioConverter alloc] initFromFormat:inFmt toFormat:outFmt_];
            if (!converter_) return fail(Error::DeviceError);

            const double ratio = static_cast<double>(format_.sampleRateHz) / inFmt.sampleRate;
            // The tap is removed in stop() before this object is destroyed, so
            // capturing the raw `this` pointer in the block is safe.
            MacAudioCapture* self = this;
            AVAudioFormat* outFmt = outFmt_;
            AVAudioConverter* conv = converter_;

            [input installTapOnBus:0
                        bufferSize:1024
                            format:inFmt
                             block:^(AVAudioPCMBuffer* inBuf, AVAudioTime* /*when*/) {
                if (inBuf.frameLength == 0) return;
                const AVAudioFrameCount cap =
                    static_cast<AVAudioFrameCount>(inBuf.frameLength * ratio) + 256;
                AVAudioPCMBuffer* outBuf =
                    [[AVAudioPCMBuffer alloc] initWithPCMFormat:outFmt frameCapacity:cap];

                __block BOOL provided = NO;
                AVAudioConverterInputBlock inputBlock =
                    ^AVAudioPCMBuffer*(AVAudioPacketCount /*n*/, AVAudioConverterInputStatus* status) {
                        if (provided) {
                            *status = AVAudioConverterInputStatus_NoDataNow;
                            return nil;
                        }
                        provided = YES;
                        *status = AVAudioConverterInputStatus_HaveData;
                        return inBuf;
                    };

                NSError* err = nil;
                [conv convertToBuffer:outBuf error:&err withInputFromBlock:inputBlock];
                if (err || outBuf.frameLength == 0) return;

                const int16_t* samples = outBuf.int16ChannelData[0];  // interleaved
                const std::size_t count =
                    static_cast<std::size_t>(outBuf.frameLength) * outFmt.channelCount;
                self->onConverted(samples, count);
            }];

            [engine_ prepare];
            NSError* startErr = nil;
            if (![engine_ startAndReturnError:&startErr]) {
                [input removeTapOnBus:0];
                engine_ = nil;
                return fail(Error::DeviceError);
            }
        }
        capturing_ = true;
        return ok();
    }

    Status stop() override {
        if (!capturing_) return ok();
        @autoreleasepool {
            [engine_.inputNode removeTapOnBus:0];
            [engine_ stop];
            engine_ = nil;
            converter_ = nil;
        }
        capturing_ = false;
        return ok();
    }

    bool isCapturing() const override { return capturing_; }

private:
    // Append converted interleaved int16 samples and emit whole 20 ms frames.
    void onConverted(const int16_t* samples, std::size_t count) {
        accum_.insert(accum_.end(), samples, samples + count);
        const int spf = samplesPerFrame(format_.sampleRateHz);
        const std::size_t need = static_cast<std::size_t>(spf) * format_.channels;
        while (accum_.size() >= need) {
            auto buf = pool_->acquire();
            if (!buf) {  // pool exhausted: drop the oldest frame to stay real-time
                accum_.erase(accum_.begin(), accum_.begin() + need);
                continue;
            }
            AudioFrame frame =
                AudioFrame::makePcm(buf, format_, spf, MediaClock::now());
            if (frame.valid()) {
                std::memcpy(frame.samples, accum_.data(), need * sizeof(int16_t));
                frame.rtpTimestamp = static_cast<uint32_t>(samplesEmitted_);
                cb_(frame);
            }
            samplesEmitted_ += spf;  // per-channel sample count -> 48 kHz RTP clock
            accum_.erase(accum_.begin(), accum_.begin() + need);
        }
    }

    AudioFormat format_{};
    FrameCallback cb_;
    std::shared_ptr<FramePool> pool_;
    std::vector<int16_t> accum_;         // leftover converted samples (audio thread)
    uint64_t samplesEmitted_ = 0;        // per-channel; wraps into uint32 rtpTimestamp
    std::atomic<bool> capturing_{false};

    AVAudioEngine* engine_ = nil;
    AVAudioConverter* converter_ = nil;
    AVAudioFormat* outFmt_ = nil;
};

// ===========================================================================
// Playback
// ===========================================================================

class MacAudioPlayback final : public AudioPlayback {
public:
    ~MacAudioPlayback() override { (void)stop(); }

    Status start(AudioFormat format) override {
        if (playing_) return fail(Error::AlreadyRunning);
        if (format.sampleRateHz != 48000 || format.channels < 1 || format.channels > 2)
            return fail(Error::InvalidArgument);
        format_ = format;
        {
            std::lock_guard<std::mutex> lk(ringMu_);
            ring_.clear();
        }

        @autoreleasepool {
            engine_ = [[AVAudioEngine alloc] init];
            AVAudioFormat* fmt = [[AVAudioFormat alloc]
                initWithCommonFormat:AVAudioPCMFormatFloat32
                          sampleRate:format_.sampleRateHz
                            channels:static_cast<AVAudioChannelCount>(format_.channels)
                         interleaved:NO];

            const int channels = format_.channels;
            MacAudioPlayback* self = this;
            srcNode_ = [[AVAudioSourceNode alloc] initWithFormat:fmt
                renderBlock:^OSStatus(BOOL* isSilence, const AudioTimeStamp* /*ts*/,
                                      AVAudioFrameCount frameCount, AudioBufferList* abl) {
                    return self->render(isSilence, frameCount, abl, channels);
                }];

            [engine_ attachNode:srcNode_];
            [engine_ connect:srcNode_ to:engine_.mainMixerNode format:fmt];
            [engine_ prepare];
            NSError* err = nil;
            if (![engine_ startAndReturnError:&err]) {
                engine_ = nil;
                srcNode_ = nil;
                return fail(Error::DeviceError);
            }
        }
        playing_ = true;
        return ok();
    }

    void enqueue(const AudioFrame& frame) override {
        if (!frame.valid()) return;
        const std::size_t count =
            static_cast<std::size_t>(frame.sampleCount) * frame.format.channels;
        std::lock_guard<std::mutex> lk(ringMu_);
        // Cap latency: if the consumer fell behind, drop the oldest audio.
        constexpr std::size_t kMaxRing = 48000 * 2 / 2;  // ~0.5 s stereo
        if (ring_.size() + count > kMaxRing) {
            const std::size_t over = ring_.size() + count - kMaxRing;
            ring_.erase(ring_.begin(), ring_.begin() + std::min(over, ring_.size()));
        }
        ring_.insert(ring_.end(), frame.samples, frame.samples + count);
    }

    Status stop() override {
        if (!playing_) return ok();
        @autoreleasepool {
            [engine_ stop];
            engine_ = nil;
            srcNode_ = nil;
        }
        playing_ = false;
        return ok();
    }

private:
    // Pull `frameCount` frames of non-interleaved float from the ring buffer.
    OSStatus render(BOOL* isSilence, AVAudioFrameCount frameCount,
                    AudioBufferList* abl, int channels) {
        std::lock_guard<std::mutex> lk(ringMu_);
        const std::size_t want = static_cast<std::size_t>(frameCount) * channels;
        const std::size_t have = std::min(want, ring_.size());
        const AVAudioFrameCount framesHave =
            static_cast<AVAudioFrameCount>(have / channels);

        for (int c = 0; c < channels && c < static_cast<int>(abl->mNumberBuffers); ++c) {
            float* out = static_cast<float*>(abl->mBuffers[c].mData);
            for (AVAudioFrameCount f = 0; f < frameCount; ++f) {
                if (f < framesHave) {
                    const int16_t s = ring_[f * channels + c];
                    out[f] = static_cast<float>(s) / 32768.0f;
                } else {
                    out[f] = 0.0f;  // underrun -> silence
                }
            }
        }
        if (framesHave > 0) {
            ring_.erase(ring_.begin(),
                        ring_.begin() + static_cast<std::ptrdiff_t>(framesHave) * channels);
        }
        if (isSilence) *isSilence = (framesHave == 0);
        return noErr;
    }

    AudioFormat format_{};
    std::mutex ringMu_;
    std::deque<int16_t> ring_;  // interleaved int16 pending playout
    std::atomic<bool> playing_{false};

    AVAudioEngine* engine_ = nil;
    AVAudioSourceNode* srcNode_ = nil;
};

std::unique_ptr<AudioCapture> createAudioCapture() {
    return std::make_unique<MacAudioCapture>();
}
std::unique_ptr<AudioPlayback> createAudioPlayback() {
    return std::make_unique<MacAudioPlayback>();
}

}  // namespace vc::audio

#endif  // __APPLE__
