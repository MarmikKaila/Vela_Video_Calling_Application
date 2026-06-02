#include "codec/VideoEncoder.h"

#include <cstring>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

namespace vc {

void VideoEncoder::AVCodecContextDeleter::operator()(AVCodecContext* c) const noexcept {
    if (c) avcodec_free_context(&c);
}
void VideoEncoder::AVFrameDeleter::operator()(AVFrame* f) const noexcept {
    if (f) av_frame_free(&f);
}
void VideoEncoder::AVPacketDeleter::operator()(AVPacket* p) const noexcept {
    if (p) av_packet_free(&p);
}

VideoEncoder::VideoEncoder() = default;
VideoEncoder::~VideoEncoder() = default;

Status VideoEncoder::open(const Config& config) {
    if (config.width <= 0 || config.height <= 0 || config.fps <= 0 ||
        config.bitrateKbps <= 0) {
        return fail(Error::InvalidArgument);
    }
    // H.264 4:2:0 requires even dimensions (chroma is half-resolution).
    if ((config.width & 1) || (config.height & 1)) return fail(Error::InvalidArgument);

    // Tear down any previous state so open() is idempotent / reconfigurable.
    ctx_.reset();
    frame_.reset();
    packet_.reset();
    pts_ = 0;
    forceKeyframe_ = false;

    const AVCodec* codec = avcodec_find_encoder_by_name("libx264");
    if (!codec) return fail(Error::Unsupported);

    ctx_.reset(avcodec_alloc_context3(codec));
    if (!ctx_) return fail(Error::CodecError);

    AVCodecContext* c = ctx_.get();
    c->width = config.width;
    c->height = config.height;
    c->pix_fmt = AV_PIX_FMT_YUV420P; // matches pipeline I420
    // Time base = 1/fps so PTS counts frames; framerate informs rate control.
    c->time_base = AVRational{1, config.fps};
    c->framerate = AVRational{config.fps, 1};
    c->bit_rate = static_cast<int64_t>(config.bitrateKbps) * 1000;
    c->rc_max_rate = c->bit_rate;
    c->rc_buffer_size = static_cast<int>(c->bit_rate); // ~1s VBV for low latency

    // Bounded GOP (~2s) so a fresh IDR caps error propagation; no B-frames so
    // there is no reordering delay (decode order == display order).
    c->gop_size = config.fps * 2;
    c->max_b_frames = 0;
    c->has_b_frames = 0;

    // libx264 real-time knobs.
    av_opt_set(c->priv_data, "preset", "superfast", 0);
    av_opt_set(c->priv_data, "tune", "zerolatency", 0);
    // Annex-B with in-band SPS/PPS so every IDR is independently decodable.
    av_opt_set(c->priv_data, "repeat-headers", "1", 0);

    if (avcodec_open2(c, codec, nullptr) < 0) {
        ctx_.reset();
        return fail(Error::CodecError);
    }

    frame_.reset(av_frame_alloc());
    packet_.reset(av_packet_alloc());
    if (!frame_ || !packet_) {
        ctx_.reset();
        return fail(Error::CodecError);
    }
    frame_->format = AV_PIX_FMT_YUV420P;
    frame_->width = config.width;
    frame_->height = config.height;

    config_ = config;
    return ok();
}

void VideoEncoder::setBitrate(int bitrateKbps) {
    if (!ctx_ || bitrateKbps <= 0) return;
    const int64_t bps = static_cast<int64_t>(bitrateKbps) * 1000;
    ctx_->bit_rate = bps;
    ctx_->rc_max_rate = bps;
    ctx_->rc_buffer_size = static_cast<int>(bps);
    config_.bitrateKbps = bitrateKbps;
}

Result<EncodedFrame, Error> VideoEncoder::encode(const VideoFrame& in) {
    if (!ctx_) return fail(Error::NotInitialized);
    if (!in.valid() || in.format != PixelFormat::I420) return fail(Error::InvalidArgument);
    if (in.width != config_.width || in.height != config_.height) {
        return fail(Error::InvalidArgument);
    }

    AVFrame* f = frame_.get();
    // Point the AVFrame at the caller's plane memory (no copy). The encoder
    // reads from these during avcodec_send_frame(); the VideoFrame outlives the
    // call, so referencing its buffer is safe.
    f->data[0] = in.planes[0].data;
    f->data[1] = in.planes[1].data;
    f->data[2] = in.planes[2].data;
    f->linesize[0] = in.planes[0].stride;
    f->linesize[1] = in.planes[1].stride;
    f->linesize[2] = in.planes[2].stride;
    f->pts = pts_++;

    // Forcing pict_type to I makes libx264 emit an IDR for this frame; the
    // AV_FRAME_FLAG_KEY hint is set too where the FFmpeg version exposes it
    // (>= libavutil 58.7 / FFmpeg 6.1). Older FFmpeg used frame->key_frame.
    if (forceKeyframe_) {
        f->pict_type = AV_PICTURE_TYPE_I;
#if defined(AV_FRAME_FLAG_KEY)
        f->flags |= AV_FRAME_FLAG_KEY;
#else
        f->key_frame = 1;
#endif
        forceKeyframe_ = false;
    } else {
        f->pict_type = AV_PICTURE_TYPE_NONE;
#if defined(AV_FRAME_FLAG_KEY)
        f->flags &= ~AV_FRAME_FLAG_KEY;
#else
        f->key_frame = 0;
#endif
    }

    if (avcodec_send_frame(ctx_.get(), f) < 0) return fail(Error::CodecError);

    AVPacket* pkt = packet_.get();
    const int ret = avcodec_receive_packet(ctx_.get(), pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        // With zerolatency + no B-frames the encoder should emit one packet per
        // frame; if it momentarily buffers, surface it as a recoverable error so
        // the caller can simply drop this frame.
        return fail(Error::CodecError);
    }
    if (ret < 0) return fail(Error::CodecError);

    EncodedFrame out;
    out.kind = MediaKind::Video;
    out.videoCodec = VideoCodec::H264;
    out.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
    out.rtpTimestamp = in.rtpTimestamp;
    out.captureTime = in.captureTime;

    std::vector<uint8_t> bytes(static_cast<std::size_t>(pkt->size));
    std::memcpy(bytes.data(), pkt->data, static_cast<std::size_t>(pkt->size));
    out.setPayload(std::move(bytes));

    av_packet_unref(pkt);
    return out;
}

} // namespace vc
