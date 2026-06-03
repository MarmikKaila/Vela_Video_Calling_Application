// loopback_call — the closest thing to a real call you can run on one machine.
//
// Left tile  ("You"):    your camera, raw (local self-view).
// Right tile ("Remote"): the SAME camera feed after a full round trip through
//                        H.264 encode -> RTP packetize -> JitterBuffer ->
//                        H.264 decode -> render. That is precisely what a remote
//                        participant would receive over the network, minus the
//                        socket. Watch the right tile lag the left by the jitter
//                        delay + codec latency — that gap IS the glass-to-glass
//                        latency of the pipeline.
//
// Threading: the capture device delivers frames on its own thread; we render the
// self-view and feed the SendPipeline there. Decoded "remote" frames are pulled
// on the GUI thread by a QTimer driving ReceivePipeline::tick(). VideoWidget's
// setFrame is thread-safe; the encoder is touched only on the capture thread and
// the decoder only on the GUI thread.

#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>

#include "capture/CaptureDevice.h"
#include "codec/VideoDecoder.h"
#include "codec/VideoEncoder.h"
#include "common/Clock.h"
#include "media/MediaPipeline.h"
#include "ui/MainWindow.h"
#include "ui/VideoWidget.h"

using namespace vc;

int main(int argc, char** argv) {
    // The YUV->RGB shader is GLSL 330 core, so request a 3.3 core-profile
    // surface for every QOpenGLWidget before the application is created.
    // Without this, macOS hands out a legacy 2.1 context, the shader fails to
    // link, and the video tiles render blank/white.
    QSurfaceFormat glFmt;
    glFmt.setProfile(QSurfaceFormat::CoreProfile);
    glFmt.setVersion(3, 3);
    glFmt.setDepthBufferSize(0);
    glFmt.setStencilBufferSize(0);
    QSurfaceFormat::setDefaultFormat(glFmt);

    QApplication app(argc, argv);

    ui::MainWindow win;
    win.setWindowTitle("Loopback Call — You (left) vs Remote-through-pipeline (right)");
    win.resize(1300, 520);
    ui::VideoWidget* selfTile = win.addParticipant("you");
    ui::VideoWidget* remoteTile = win.addParticipant("remote");
    QObject::connect(&win, &ui::MainWindow::leaveRequested, &app, &QApplication::quit);
    win.show();

    // Codecs + pipeline are created lazily on the first frame, because the real
    // capture resolution isn't known until the camera delivers a frame.
    auto enc = std::make_shared<VideoEncoder>();
    auto dec = std::make_shared<VideoDecoder>();
    std::shared_ptr<SendPipeline> send;
    std::shared_ptr<ReceivePipeline> recv;
    std::mutex initMu;
    std::atomic<bool> ready{false};

    auto devices = enumerateCaptureDevices();
    if (devices.empty()) {
        qFatal("No camera found. (Grant Terminal camera permission in System "
               "Settings > Privacy & Security > Camera.)");
    }
    auto capture = createCaptureDevice();

    VideoFormat fmt;
    fmt.resolution = {640, 480};
    fmt.fpsNumerator = 30;
    fmt.fpsDenominator = 1;
    fmt.pixelFormat = PixelFormat::I420;
    if (!capture->open(devices[0], fmt)) {
        qFatal("Failed to open the camera.");
    }

    auto onFrame = [&](const VideoFrame& frame) {
        selfTile->setFrame(frame); // local self-view (thread-safe)

        if (!ready.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(initMu);
            if (!ready.load(std::memory_order_relaxed)) {
                VideoEncoder::Config c;
                c.width = frame.width;
                c.height = frame.height;
                c.fps = 30;
                c.bitrateKbps = 1500;
                if (!enc->open(c) || !dec->open()) return;

                ReceivePipeline::Config rc;
                rc.videoSsrc = 0x1;
                rc.targetDelay = std::chrono::milliseconds(20);
                recv = std::make_shared<ReceivePipeline>(
                    dec.get(), nullptr, rc,
                    [remoteTile](const VideoFrame& decoded) { remoteTile->setFrame(decoded); });

                SendPipeline::Config sc;
                sc.videoSsrc = 0x1;
                send = std::make_shared<SendPipeline>(
                    enc.get(), nullptr,
                    [&](const RtpPacket& pkt) { recv->pushPacket(pkt, MediaClock::now()); }, sc);

                ready.store(true, std::memory_order_release);
            }
        }
        if (send) (void)send->pushVideoFrame(frame); // encode -> RTP -> jitter
    };

    if (!capture->start(onFrame)) {
        qFatal("Failed to start the camera.");
    }

    // Pull decoded "remote" frames out of the jitter buffer ~60x/sec on the GUI thread.
    QTimer playout;
    QObject::connect(&playout, &QTimer::timeout, [&]() {
        if (recv) recv->tick(MediaClock::now());
    });
    playout.start(16);

    const int code = app.exec();
    (void)capture->stop();
    capture->close();
    return code;
}
