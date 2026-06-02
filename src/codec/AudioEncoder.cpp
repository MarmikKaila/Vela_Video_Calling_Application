#include "codec/AudioEncoder.h"

#include <opus.h>

namespace vc {

namespace {
// Opus max packet payload is bounded; 4000 bytes comfortably covers any single
// 48 kHz frame at conferencing bitrates.
constexpr std::size_t kMaxPacket = 4000;

// Valid Opus frame sizes at 48 kHz (samples/channel): 2.5/5/10/20/40/60 ms.
bool isValidFrameSize(int samplesPerChannel) {
    switch (samplesPerChannel) {
        case 120:   // 2.5 ms
        case 240:   // 5 ms
        case 480:   // 10 ms
        case 960:   // 20 ms
        case 1920:  // 40 ms
        case 2880:  // 60 ms
            return true;
        default:
            return false;
    }
}
} // namespace

void AudioEncoder::OpusEncoderDeleter::operator()(OpusEncoder* e) const noexcept {
    if (e) opus_encoder_destroy(e);
}

AudioEncoder::AudioEncoder() = default;
AudioEncoder::~AudioEncoder() = default;

Status AudioEncoder::open(AudioFormat format, int bitrateBps) {
    if (format.sampleRateHz != 48000) return fail(Error::Unsupported);
    if (format.channels != 1 && format.channels != 2) return fail(Error::InvalidArgument);
    if (bitrateBps <= 0) return fail(Error::InvalidArgument);

    enc_.reset();

    int err = OPUS_OK;
    OpusEncoder* e = opus_encoder_create(format.sampleRateHz, format.channels,
                                         OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !e) return fail(Error::CodecError);
    enc_.reset(e);

    opus_encoder_ctl(enc_.get(), OPUS_SET_BITRATE(bitrateBps));

    format_ = format;
    scratch_.resize(kMaxPacket);
    return ok();
}

Result<EncodedFrame, Error> AudioEncoder::encode(const AudioFrame& in) {
    if (!enc_) return fail(Error::NotInitialized);
    if (!in.valid()) return fail(Error::InvalidArgument);
    if (in.format.sampleRateHz != format_.sampleRateHz ||
        in.format.channels != format_.channels) {
        return fail(Error::InvalidArgument);
    }
    if (!isValidFrameSize(in.sampleCount)) return fail(Error::InvalidArgument);

    const opus_int32 n = opus_encode(enc_.get(), in.samples, in.sampleCount,
                                     scratch_.data(),
                                     static_cast<opus_int32>(scratch_.size()));
    if (n < 0) return fail(Error::CodecError);

    EncodedFrame out;
    out.kind = MediaKind::Audio;
    out.audioCodec = AudioCodec::Opus;
    out.keyframe = false; // not meaningful for Opus
    out.rtpTimestamp = in.rtpTimestamp;
    out.captureTime = in.captureTime;
    out.setPayload(std::vector<uint8_t>(scratch_.begin(),
                                        scratch_.begin() + n));
    return out;
}

} // namespace vc
