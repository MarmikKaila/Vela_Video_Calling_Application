// capture_dump — CLI smoke test for the capture module.
//
// Opens the default camera at 640x480, captures N frames and writes raw I420 to
// a file, then reports the frame count, resolution and average inter-frame
// interval. Designed to fail cleanly (clear message, nonzero exit) when no
// camera is available (e.g. a CI sandbox) rather than crash.
//
// Usage:  capture_dump [frames=30] [outfile=/tmp/capture.yuv]

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "capture/CaptureDevice.h"
#include "common/Clock.h"
#include "common/VideoFrame.h"

using namespace vc;

namespace {

// Append a frame's three I420 planes (tightly packed per plane) to the stream.
void writeI420(std::ofstream& os, const VideoFrame& f) {
    const int w = f.width, h = f.height;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    for (int y = 0; y < h; ++y)
        os.write(reinterpret_cast<const char*>(f.planes[0].data + y * f.planes[0].stride), w);
    for (int y = 0; y < ch; ++y)
        os.write(reinterpret_cast<const char*>(f.planes[1].data + y * f.planes[1].stride), cw);
    for (int y = 0; y < ch; ++y)
        os.write(reinterpret_cast<const char*>(f.planes[2].data + y * f.planes[2].stride), cw);
}

} // namespace

int main(int argc, char** argv) {
    const int wantFrames = argc > 1 ? std::atoi(argv[1]) : 30;
    const std::string outPath = argc > 2 ? argv[2] : "/tmp/capture.yuv";
    if (wantFrames <= 0) {
        std::fprintf(stderr, "capture_dump: frame count must be positive\n");
        return 2;
    }

    auto devices = enumerateCaptureDevices();
    std::fprintf(stderr, "capture_dump: %zu camera(s) detected\n", devices.size());
    for (const auto& d : devices)
        std::fprintf(stderr, "  - %s (%s)\n", d.displayName.c_str(), d.id.c_str());

    auto dev = createCaptureDevice();
    if (!dev) {
        std::fprintf(stderr, "capture_dump: no capture backend on this platform\n");
        return 1;
    }

    CaptureDeviceInfo target;
    if (!devices.empty()) target = devices.front();

    VideoFormat desired;
    desired.resolution = {640, 480};
    desired.fpsNumerator = 30;
    desired.fpsDenominator = 1;
    desired.pixelFormat = PixelFormat::I420;

    if (auto s = dev->open(target, desired); !s) {
        std::fprintf(stderr,
                     "capture_dump: failed to open camera (%s). No usable camera "
                     "available — exiting.\n",
                     std::string(to_string(s.error())).c_str());
        return 1;
    }

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::fprintf(stderr, "capture_dump: cannot open output file %s\n", outPath.c_str());
        dev->close();
        return 1;
    }

    std::mutex mu;
    std::condition_variable cv;
    int captured = 0;
    int frameW = 0, frameH = 0;
    std::vector<SteadyTime> stamps;
    stamps.reserve(wantFrames);

    auto onFrame = [&](const VideoFrame& f) {
        std::lock_guard<std::mutex> lk(mu);
        if (captured >= wantFrames) return;
        if (frameW == 0) { frameW = f.width; frameH = f.height; }
        writeI420(out, f);
        stamps.push_back(f.captureTime);
        if (++captured >= wantFrames) cv.notify_one();
    };

    if (auto s = dev->start(onFrame); !s) {
        std::fprintf(stderr, "capture_dump: failed to start capture (%s)\n",
                     std::string(to_string(s.error())).c_str());
        dev->close();
        return 1;
    }

    {
        std::unique_lock<std::mutex> lk(mu);
        const bool done = cv.wait_for(lk, std::chrono::seconds(15),
                                      [&] { return captured >= wantFrames; });
        if (!done) {
            std::fprintf(stderr,
                         "capture_dump: timed out after %d/%d frames\n",
                         captured, wantFrames);
        }
    }

    (void)dev->stop();
    dev->close();
    out.flush();
    out.close();

    if (captured == 0) {
        std::fprintf(stderr, "capture_dump: captured 0 frames — no camera output\n");
        return 1;
    }

    // Average inter-frame interval over captured timestamps.
    double avgMs = 0.0;
    if (stamps.size() >= 2) {
        const auto total = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
                               stamps.back() - stamps.front())
                               .count();
        avgMs = total / static_cast<double>(stamps.size() - 1);
    }

    const std::size_t expectedBytes =
        VideoFrame::i420Size(frameW, frameH) * static_cast<std::size_t>(captured);

    std::printf("capture_dump: wrote %d frame(s) to %s\n", captured, outPath.c_str());
    std::printf("  resolution : %dx%d (I420)\n", frameW, frameH);
    std::printf("  bytes      : %zu (= %dx%d*3/2 * %d frames)\n",
                expectedBytes, frameW, frameH, captured);
    std::printf("  avg interval: %.2f ms (%.1f fps)\n", avgMs,
                avgMs > 0 ? 1000.0 / avgMs : 0.0);
    return 0;
}
