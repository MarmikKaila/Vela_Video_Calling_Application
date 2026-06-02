#pragma once

// CaptureDevice — abstract camera capture interface (Phase 1 contract).
//
// One interface, three backends: MacCapture (AVFoundation), LinuxCapture
// (V4L2), WindowsCapture (DirectShow/Media Foundation). Nothing above this
// layer knows which platform it is on — they all consume VideoFrames delivered
// through the FrameCallback.
//
// Lifecycle:  enumerate → create → open(format) → start(cb) → … → stop → close
//
// Threading contract: start() spins up the backend's own capture thread; the
// FrameCallback is invoked ON THAT THREAD. Implementations deliver pooled,
// already-I420 frames (converting from the device's native format if needed) so
// downstream code is platform- and format-agnostic. Keep callback work short —
// hand the frame to a queue and return; do not block the capture thread.

#include <functional>
#include <string>
#include <vector>

#include "common/Error.h"
#include "common/Types.h"
#include "common/VideoFrame.h"

namespace vc {

// Identifies a physical camera. `id` is an opaque, platform-specific handle
// (V4L2 node path, AVFoundation unique ID, DirectShow moniker).
struct CaptureDeviceInfo {
    std::string id;
    std::string displayName;
};

class CaptureDevice {
public:
    using FrameCallback = std::function<void(const VideoFrame&)>;

    virtual ~CaptureDevice() = default;

    // Capabilities the opened device advertises. Empty until open() succeeds on
    // backends that probe formats lazily.
    [[nodiscard]] virtual std::vector<VideoFormat> supportedFormats() const = 0;

    // Reserve the device and select a capture format. `desired` is a hint; the
    // backend picks the closest supported format and reports the actual choice
    // via supportedFormats()/the frames it emits.
    [[nodiscard]] virtual Status open(const CaptureDeviceInfo& device,
                                      const VideoFormat& desired) = 0;

    // Begin delivering frames to `onFrame` on the capture thread.
    [[nodiscard]] virtual Status start(FrameCallback onFrame) = 0;

    // Stop delivery; safe to call start() again afterwards.
    [[nodiscard]] virtual Status stop() = 0;

    // Release the device. Implicitly stops if still capturing.
    virtual void close() = 0;

    [[nodiscard]] virtual bool isCapturing() const = 0;
};

// --- Platform factory (implemented per-OS in capture/<Platform>Capture.cpp) ---

// List cameras available on this machine.
[[nodiscard]] std::vector<CaptureDeviceInfo> enumerateCaptureDevices();

// Construct the capture backend for the current platform. Never null.
[[nodiscard]] std::unique_ptr<CaptureDevice> createCaptureDevice();

} // namespace vc
