// LinuxCapture.cpp — V4L2 backend stub. Compiled only on Linux; every method
// reports Error::Unsupported until the real implementation lands.

#include "capture/LinuxCapture.h"

#if defined(__linux__)

namespace vc {

std::vector<VideoFormat> LinuxCapture::supportedFormats() const { return {}; }

Status LinuxCapture::open(const CaptureDeviceInfo&, const VideoFormat&) {
    return fail(Error::Unsupported);
}

Status LinuxCapture::start(FrameCallback) { return fail(Error::Unsupported); }

Status LinuxCapture::stop() { return fail(Error::Unsupported); }

void LinuxCapture::close() {}

bool LinuxCapture::isCapturing() const { return false; }

} // namespace vc

#endif // __linux__
