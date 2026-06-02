#include "codec/AudioDecoder.h"

#include <opus.h>

namespace vc {

namespace {
// Opus can output up to 120 ms per packet at 48 kHz = 5760 samples/channel.
constexpr int kMaxSamplesPerChannel = 5760;
// A few buffers in flight (decoder reuse + playout/jitter buffer holding on).
constexpr std::size_t kPoolDepth = 8;
} // namespace

void AudioDecoder::OpusDecoderDeleter::operator()(OpusDecoder* d) const noexcept {
    if (d) opus_decoder_destroy(d);
}

AudioDecoder::AudioDecoder() = default;
AudioDecoder::~AudioDecoder() = default;

Status AudioDecoder::open(AudioFormat format) {
    if (format.sampleRateHz != 48000) return fail(Error::Unsupported);
    if (format.channels != 1 && format.channels != 2) return fail(Error::InvalidArgument);

    dec_.reset();
    pool_.reset();

    int err = OPUS_OK;
    OpusDecoder* d = opus_decoder_create(format.sampleRateHz, format.channels, &err);
    if (err != OPUS_OK || !d) return fail(Error::CodecError);
    dec_.reset(d);

    format_ = format;
    const std::size_t maxBytes = static_cast<std::size_t>(kMaxSamplesPerChannel) *
                                 format.channels * sizeof(int16_t);
    pool_ = FramePool::create(maxBytes, kPoolDepth);
    return ok();
}

Result<AudioFrame, Error> AudioDecoder::decode(const EncodedFrame& in) {
    if (!dec_) return fail(Error::NotInitialized);
    if (in.empty()) return fail(Error::InvalidArgument);

    auto buf = pool_->acquire();
    if (!buf) return fail(Error::ResourceExhausted);

    // Decode straight into the pooled buffer. makePcm validates capacity, but we
    // must decode first to learn the sample count, so write to the raw buffer
    // and then describe it with makePcm.
    int16_t* pcm = reinterpret_cast<int16_t*>(buf->data());
    const int decoded = opus_decode(dec_.get(), in.data(),
                                    static_cast<opus_int32>(in.size()), pcm,
                                    kMaxSamplesPerChannel, /*decode_fec=*/0);
    if (decoded < 0) return fail(Error::CodecError);

    AudioFrame out = AudioFrame::makePcm(std::move(buf), format_, decoded, in.captureTime);
    if (!out.valid()) return fail(Error::Internal);
    out.rtpTimestamp = in.rtpTimestamp;
    return out;
}

} // namespace vc
