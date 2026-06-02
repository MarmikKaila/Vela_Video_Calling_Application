#pragma once

// MacCapture — AVFoundation camera backend for macOS.
//
// Implements CaptureDevice on top of AVCaptureSession + AVCaptureVideoDataOutput.
// The session is configured to deliver NV12 (kCVPixelFormatType_420YpCbCr8Bi-
// PlanarVideoRange) when the device supports it, otherwise 32BGRA. Each captured
// sample buffer is converted to canonical I420 into a buffer borrowed from a
// pre-allocated FramePool (no malloc on the capture thread) and handed to the
// FrameCallback ON the AVFoundation delivery (dispatch) thread, per the
// CaptureDevice threading contract.
//
// All AVFoundation/Objective-C state is hidden behind an opaque pImpl so this
// header is a clean C++ surface includable from plain .cpp translation units
// (e.g. CaptureFactory.cpp). The implementation lives in MacCapture.mm.
//
// Lifecycle: open(device, desired) reserves the camera and selects the closest
// supported format; start(cb) begins delivery; stop() halts it (restartable);
// close() tears the session down. The class is not copyable.

#include <memory>

#include "capture/CaptureDevice.h"

#if defined(__APPLE__)

namespace vc {

// List cameras visible to AVFoundation. Defined here (used by CaptureFactory).
[[nodiscard]] std::vector<CaptureDeviceInfo> macEnumerateCaptureDevices();

class MacCapture final : public CaptureDevice {
public:
    MacCapture();
    ~MacCapture() override;

    MacCapture(const MacCapture&) = delete;
    MacCapture& operator=(const MacCapture&) = delete;

    [[nodiscard]] std::vector<VideoFormat> supportedFormats() const override;
    [[nodiscard]] Status open(const CaptureDeviceInfo& device,
                              const VideoFormat& desired) override;
    [[nodiscard]] Status start(FrameCallback onFrame) override;
    [[nodiscard]] Status stop() override;
    void close() override;
    [[nodiscard]] bool isCapturing() const override;

private:
    struct Impl;            // opaque — holds the Objective-C session/delegate
    std::unique_ptr<Impl> impl_;
};

} // namespace vc

#endif // __APPLE__
