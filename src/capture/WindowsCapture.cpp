// WindowsCapture.cpp — Media Foundation backend stub. Compiled only on Windows;
// every method reports Error::Unsupported until the real implementation lands.

#include "capture/WindowsCapture.h"

#if defined(_WIN32)

namespace vc {

std::vector<VideoFormat> WindowsCapture::supportedFormats() const { return {}; }

Status WindowsCapture::open(const CaptureDeviceInfo&, const VideoFormat&) {
    return fail(Error::Unsupported);
}

Status WindowsCapture::start(FrameCallback) { return fail(Error::Unsupported); }

Status WindowsCapture::stop() { return fail(Error::Unsupported); }

void WindowsCapture::close() {}

bool WindowsCapture::isCapturing() const { return false; }

} // namespace vc

#endif // _WIN32
