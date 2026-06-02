#pragma once

// LinuxCapture — V4L2 camera backend (STUB).
//
// Placeholder so the CaptureDevice interface is visibly tri-platform. A real
// implementation would open a /dev/videoN node, negotiate a format via VIDIOC_*
// ioctls, mmap buffers and pump them through a capture thread, converting YUYV/
// MJPEG to I420. Until then every method returns Error::Unsupported.

#include "capture/CaptureDevice.h"

#if defined(__linux__)

namespace vc {

class LinuxCapture final : public CaptureDevice {
public:
    [[nodiscard]] std::vector<VideoFormat> supportedFormats() const override;
    [[nodiscard]] Status open(const CaptureDeviceInfo& device,
                              const VideoFormat& desired) override;
    [[nodiscard]] Status start(FrameCallback onFrame) override;
    [[nodiscard]] Status stop() override;
    void close() override;
    [[nodiscard]] bool isCapturing() const override;
};

} // namespace vc

#endif // __linux__
