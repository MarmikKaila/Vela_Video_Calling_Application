// MacCapture.mm — AVFoundation backend implementation (Objective-C++).
//
// See MacCapture.h for the contract. This file owns all the Objective-C state:
// an AVCaptureSession, an AVCaptureDeviceInput, an AVCaptureVideoDataOutput and
// a sample-buffer delegate. Captured CVPixelBuffers (NV12 or BGRA) are converted
// to I420 into FramePool buffers and delivered on AVFoundation's dispatch queue.

#include "capture/MacCapture.h"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "capture/PixelConvert.h"
#include "common/Clock.h"
#include "common/FramePool.h"
#include "common/VideoFrame.h"

namespace vc {

namespace {

// Number of pooled buffers in flight. A handful is plenty: capture produces at
// the camera's frame rate and downstream is expected to release promptly.
constexpr std::size_t kPoolDepth = 8;

// Map an AVCaptureDevice to our platform-neutral info struct.
CaptureDeviceInfo infoFromDevice(AVCaptureDevice* dev) {
    CaptureDeviceInfo info;
    info.id = std::string(dev.uniqueID.UTF8String ? dev.uniqueID.UTF8String : "");
    info.displayName =
        std::string(dev.localizedName.UTF8String ? dev.localizedName.UTF8String : "");
    return info;
}

AVCaptureDeviceDiscoverySession* discovery() {
    NSArray<AVCaptureDeviceType>* types = @[
        AVCaptureDeviceTypeBuiltInWideAngleCamera,
#if defined(__MAC_14_0) || defined(__IPHONE_17_0)
        AVCaptureDeviceTypeExternal,
#endif
    ];
    return [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:types
                              mediaType:AVMediaTypeVideo
                               position:AVCaptureDevicePositionUnspecified];
}

} // namespace

std::vector<CaptureDeviceInfo> macEnumerateCaptureDevices() {
    std::vector<CaptureDeviceInfo> out;
    @autoreleasepool {
        for (AVCaptureDevice* dev in discovery().devices) {
            out.push_back(infoFromDevice(dev));
        }
    }
    return out;
}

} // namespace vc

// ---- Sample buffer delegate ------------------------------------------------

@interface VCCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@end

@implementation VCCaptureDelegate {
@public
    std::shared_ptr<vc::FramePool> _pool;
    vc::CaptureDevice::FrameCallback _cb;
    std::mutex _cbMu; // guards _cb so stop() can clear it safely
    // RTP-timestamp epoch: the capture instant of the first delivered frame.
    // Subsequent frames' 90 kHz RTP timestamps are measured relative to it so
    // they advance monotonically (the jitter buffer keys frames by timestamp).
    // Frames arrive on a serial delivery queue, so no extra locking is needed.
    vc::SteadyTime _rtpEpoch;
    bool _haveRtpEpoch;
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;

    vc::CaptureDevice::FrameCallback cb;
    std::shared_ptr<vc::FramePool> pool;
    {
        std::lock_guard<std::mutex> lk(_cbMu);
        cb = _cb;
        pool = _pool;
    }
    if (!cb || !pool) return;

    CVImageBufferRef img = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!img) return;

    const int w = static_cast<int>(CVPixelBufferGetWidth(img));
    const int h = static_cast<int>(CVPixelBufferGetHeight(img));
    if (w <= 0 || h <= 0) return;

    auto buf = pool->acquire();
    if (!buf) return; // back-pressure: pool exhausted, drop this frame

    const vc::SteadyTime captured = vc::MediaClock::now();
    vc::VideoFrame frame = vc::VideoFrame::makeI420(buf, w, h, captured);
    if (!frame.valid()) return;

    // Assign the 90 kHz RTP timestamp at capture (the contract in VideoFrame.h),
    // measured from the first frame's instant. Without this every frame would
    // carry timestamp 0 and the receiver's jitter buffer would collapse them all
    // into one access unit, then reject the rest as "late".
    if (!_haveRtpEpoch) {
        _rtpEpoch = captured;
        _haveRtpEpoch = true;
    }
    frame.rtpTimestamp = vc::MediaClock::toRtp(captured, _rtpEpoch, vc::kVideoRtpClockHz);

    uint8_t* dst = frame.planes[0].data;

    CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    const OSType fmt = CVPixelBufferGetPixelFormatType(img);

    if (fmt == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
        fmt == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange) {
        const uint8_t* srcY =
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(img, 0));
        const int yStride =
            static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(img, 0));
        const uint8_t* srcUV =
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(img, 1));
        const int uvStride =
            static_cast<int>(CVPixelBufferGetBytesPerRowOfPlane(img, 1));
        vc::pixel::nv12ToI420(srcY, yStride, srcUV, uvStride, w, h, dst);
    } else if (fmt == kCVPixelFormatType_32BGRA) {
        const uint8_t* src =
            static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(img));
        const int stride = static_cast<int>(CVPixelBufferGetBytesPerRow(img));
        vc::pixel::bgraToI420(src, stride, w, h, dst);
    } else {
        CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
        return; // unexpected format; configured output should prevent this
    }

    CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
    cb(frame);
}

@end

// ---- MacCapture::Impl ------------------------------------------------------

namespace vc {

struct MacCapture::Impl {
    AVCaptureSession* session = nil;
    AVCaptureDeviceInput* input = nil;
    AVCaptureVideoDataOutput* output = nil;
    VCCaptureDelegate* delegate = nil;
    dispatch_queue_t queue = nullptr;

    std::shared_ptr<FramePool> pool;
    std::vector<VideoFormat> formats;
    VideoFormat active{};
    bool opened = false;
    bool capturing = false;
};

MacCapture::MacCapture() : impl_(std::make_unique<Impl>()) {}

MacCapture::~MacCapture() { close(); }

std::vector<VideoFormat> MacCapture::supportedFormats() const {
    return impl_->formats;
}

Status MacCapture::open(const CaptureDeviceInfo& device, const VideoFormat& desired) {
    if (impl_->opened) return fail(Error::AlreadyRunning);

    @autoreleasepool {
        AVCaptureDevice* avDev = nil;
        if (!device.id.empty()) {
            avDev = [AVCaptureDevice
                deviceWithUniqueID:[NSString stringWithUTF8String:device.id.c_str()]];
        }
        if (!avDev) {
            avDev = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        }
        if (!avDev) return fail(Error::NotFound);

        NSError* err = nil;
        AVCaptureDeviceInput* in =
            [AVCaptureDeviceInput deviceInputWithDevice:avDev error:&err];
        if (!in || err) return fail(Error::DeviceError);

        AVCaptureSession* sess = [[AVCaptureSession alloc] init];
        if (![sess canAddInput:in]) return fail(Error::DeviceError);
        [sess addInput:in];

        AVCaptureVideoDataOutput* out = [[AVCaptureVideoDataOutput alloc] init];
        out.alwaysDiscardsLateVideoFrames = YES;

        // Prefer NV12 (video range); fall back to BGRA. Both convert to I420.
        NSArray<NSNumber*>* avail = out.availableVideoCVPixelFormatTypes;
        OSType chosen = 0;
        for (NSNumber* n in avail) {
            if (n.unsignedIntValue == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) {
                chosen = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
                break;
            }
        }
        if (chosen == 0) {
            for (NSNumber* n in avail) {
                if (n.unsignedIntValue == kCVPixelFormatType_32BGRA) {
                    chosen = kCVPixelFormatType_32BGRA;
                    break;
                }
            }
        }
        if (chosen == 0 && avail.count > 0) {
            chosen = static_cast<OSType>(avail.firstObject.unsignedIntValue);
        }

        out.videoSettings = @{
            (id)kCVPixelBufferPixelFormatTypeKey : @(chosen),
        };
        if (![sess canAddOutput:out]) return fail(Error::DeviceError);
        [sess addOutput:out];

        // Pick a session preset close to the desired resolution. AVFoundation
        // delivers a concrete size we read back from the first frame; we record
        // the desired/active format hint here.
        const int dw = desired.resolution.width;
        const int dh = desired.resolution.height;
        if (dw >= 1920 && [sess canSetSessionPreset:AVCaptureSessionPreset1920x1080]) {
            sess.sessionPreset = AVCaptureSessionPreset1920x1080;
        } else if (dw >= 1280 && [sess canSetSessionPreset:AVCaptureSessionPreset1280x720]) {
            sess.sessionPreset = AVCaptureSessionPreset1280x720;
        } else if ([sess canSetSessionPreset:AVCaptureSessionPreset640x480]) {
            sess.sessionPreset = AVCaptureSessionPreset640x480;
        } else {
            sess.sessionPreset = AVCaptureSessionPresetMedium;
        }

        const int aw = dw > 0 ? dw : 640;
        const int ah = dh > 0 ? dh : 480;

        VideoFormat active;
        active.resolution = {aw, ah};
        active.fpsNumerator = desired.fpsNumerator > 0 ? desired.fpsNumerator : 30;
        active.fpsDenominator = desired.fpsDenominator > 0 ? desired.fpsDenominator : 1;
        active.pixelFormat = PixelFormat::I420; // we always emit I420 downstream

        // Pool sized for the largest plausible frame at the chosen preset; a
        // single allocation up front means zero malloc on the capture thread.
        const std::size_t bufSize = VideoFrame::i420Size(
            std::max(aw, 1920), std::max(ah, 1080));
        impl_->pool = FramePool::create(bufSize, kPoolDepth);

        impl_->session = sess;
        impl_->input = in;
        impl_->output = out;
        impl_->active = active;
        impl_->formats = {active};
        impl_->opened = true;
    }
    return ok();
}

Status MacCapture::start(FrameCallback onFrame) {
    if (!impl_->opened) return fail(Error::NotInitialized);
    if (impl_->capturing) return fail(Error::AlreadyRunning);
    if (!onFrame) return fail(Error::InvalidArgument);

    @autoreleasepool {
        impl_->delegate = [[VCCaptureDelegate alloc] init];
        impl_->delegate->_pool = impl_->pool;
        impl_->delegate->_cb = std::move(onFrame);
        impl_->delegate->_haveRtpEpoch = false; // fresh RTP timeline per session

        impl_->queue =
            dispatch_queue_create("vc.capture.delivery", DISPATCH_QUEUE_SERIAL);
        [impl_->output setSampleBufferDelegate:impl_->delegate queue:impl_->queue];

        [impl_->session startRunning];
        if (!impl_->session.isRunning) {
            [impl_->output setSampleBufferDelegate:nil queue:nullptr];
            impl_->delegate = nil;
            impl_->queue = nullptr;
            return fail(Error::DeviceError);
        }
        impl_->capturing = true;
    }
    return ok();
}

Status MacCapture::stop() {
    if (!impl_->capturing) return ok();
    @autoreleasepool {
        // Clear the callback first so no frame is delivered after stop returns.
        if (impl_->delegate) {
            std::lock_guard<std::mutex> lk(impl_->delegate->_cbMu);
            impl_->delegate->_cb = nullptr;
        }
        [impl_->session stopRunning];
        [impl_->output setSampleBufferDelegate:nil queue:nullptr];
        impl_->delegate = nil;
        impl_->queue = nullptr;
        impl_->capturing = false;
    }
    return ok();
}

void MacCapture::close() {
    if (!impl_) return;
    if (impl_->capturing) (void)stop();
    @autoreleasepool {
        if (impl_->session) {
            if (impl_->input) [impl_->session removeInput:impl_->input];
            if (impl_->output) [impl_->session removeOutput:impl_->output];
        }
        impl_->session = nil;
        impl_->input = nil;
        impl_->output = nil;
    }
    impl_->pool.reset();
    impl_->formats.clear();
    impl_->opened = false;
}

bool MacCapture::isCapturing() const { return impl_->capturing; }

} // namespace vc

#endif // __APPLE__
