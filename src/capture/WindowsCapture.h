#pragma once

// WindowsCapture — Media Foundation / DirectShow camera backend (STUB).
//
// Placeholder so the CaptureDevice interface is visibly tri-platform. A real
// implementation would enumerate IMFActivate video sources, drive an
// IMFSourceReader, and convert NV12/MJPEG samples to I420 on a capture thread.
// Until then every method returns Error::Unsupported.

#include "capture/CaptureDevice.h"

#if defined(_WIN32)

namespace vc {

class WindowsCapture final : public CaptureDevice {
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

#endif // _WIN32
