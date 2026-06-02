// CaptureFactory.cpp — platform dispatch for the CaptureDevice factory.
//
// enumerateCaptureDevices() and createCaptureDevice() are declared in
// CaptureDevice.h (the frozen contract); their single definition lives here and
// selects the right backend via platform #ifdefs. createCaptureDevice() never
// returns null — on platforms without a real backend it returns the stub, whose
// methods report Error::Unsupported.

#include "capture/CaptureDevice.h"

#include <memory>

#if defined(__APPLE__)
#include "capture/MacCapture.h"
#elif defined(__linux__)
#include "capture/LinuxCapture.h"
#elif defined(_WIN32)
#include "capture/WindowsCapture.h"
#endif

namespace vc {

std::vector<CaptureDeviceInfo> enumerateCaptureDevices() {
#if defined(__APPLE__)
    return macEnumerateCaptureDevices();
#else
    // Linux/Windows stubs do not enumerate yet.
    return {};
#endif
}

std::unique_ptr<CaptureDevice> createCaptureDevice() {
#if defined(__APPLE__)
    return std::make_unique<MacCapture>();
#elif defined(__linux__)
    return std::make_unique<LinuxCapture>();
#elif defined(_WIN32)
    return std::make_unique<WindowsCapture>();
#else
#error "Unsupported platform: no CaptureDevice backend"
#endif
}

} // namespace vc
