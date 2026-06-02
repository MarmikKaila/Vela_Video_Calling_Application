#include "codec/VideoDecoder.h"

#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

namespace vc {

namespace {
// A few buffers in flight (decoder reuse + downstream renderer/SFU holding on).
constexpr std::size_t kPoolDepth = 8;
} // namespace

void VideoDecoder::AVCodecContextDeleter::operator()(AVCodecContext* c) const noexcept {
    if (c) avcodec_free_context(&c);
}
void VideoDecoder::AVFrameDeleter::operator()(AVFrame* f) const noexcept {
    if (f) av_frame_free(&f);
}
void VideoDecoder::AVPacketDeleter::operator()(AVPacket* p) const noexcept {
    if (p) av_packet_free(&p);
}

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;

Status VideoDecoder::open() {
    ctx_.reset();
    frame_.reset();
    packet_.reset();
    pool_.reset();
    poolW_ = poolH_ = 0;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return fail(Error::Unsupported);

    ctx_.reset(avcodec_alloc_context3(codec));
    if (!ctx_) return fail(Error::CodecError);

    if (avcodec_open2(ctx_.get(), codec, nullptr) < 0) {
        ctx_.reset();
        return fail(Error::CodecError);
    }

    frame_.reset(av_frame_alloc());
    packet_.reset(av_packet_alloc());
    if (!frame_ || !packet_) {
        ctx_.reset();
        return fail(Error::CodecError);
    }
    return ok();
}

void VideoDecoder::ensurePool(int w, int h) {
    if (pool_ && w <= poolW_ && h <= poolH_) return;
    const int pw = w > poolW_ ? w : poolW_;
    const int ph = h > poolH_ ? h : poolH_;
    pool_ = FramePool::create(VideoFrame::i420Size(pw, ph), kPoolDepth);
    poolW_ = pw;
    poolH_ = ph;
}

Result<VideoFrame, Error> VideoDecoder::decode(const EncodedFrame& in) {
    if (!ctx_) return fail(Error::NotInitialized);
    if (in.empty()) return fail(Error::InvalidArgument);

    AVPacket* pkt = packet_.get();
    // Wrap the caller's payload without copying; the EncodedFrame outlives this
    // call so its bytes remain valid for the duration of send_packet.
    pkt->data = const_cast<uint8_t*>(in.data());
    pkt->size = static_cast<int>(in.size());

    const int sret = avcodec_send_packet(ctx_.get(), pkt);
    pkt->data = nullptr;
    pkt->size = 0;
    if (sret < 0) return fail(Error::CodecError);

    AVFrame* f = frame_.get();
    const int rret = avcodec_receive_frame(ctx_.get(), f);
    if (rret == AVERROR(EAGAIN) || rret == AVERROR_EOF) {
        // Needs more data before a picture can be produced (e.g. before the
        // first IDR is complete). Recoverable: caller feeds the next packet.
        return fail(Error::CodecError);
    }
    if (rret < 0) return fail(Error::CodecError);

    if (f->format != AV_PIX_FMT_YUV420P) {
        av_frame_unref(f);
        return fail(Error::Unsupported);
    }

    const int w = f->width;
    const int h = f->height;
    ensurePool(w, h);

    auto buf = pool_->acquire();
    if (!buf) {
        av_frame_unref(f);
        return fail(Error::ResourceExhausted);
    }

    VideoFrame out = VideoFrame::makeI420(std::move(buf), w, h, in.captureTime);
    if (!out.valid()) {
        av_frame_unref(f);
        return fail(Error::Internal);
    }
    out.rtpTimestamp = in.rtpTimestamp;

    // Copy each plane row-by-row to collapse the decoder's (possibly padded)
    // linesize into our tightly-packed pool layout.
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    av_image_copy_plane(out.planes[0].data, out.planes[0].stride,
                        f->data[0], f->linesize[0], w, h);
    av_image_copy_plane(out.planes[1].data, out.planes[1].stride,
                        f->data[1], f->linesize[1], cw, ch);
    av_image_copy_plane(out.planes[2].data, out.planes[2].stride,
                        f->data[2], f->linesize[2], cw, ch);

    av_frame_unref(f);
    return out;
}

} // namespace vc
