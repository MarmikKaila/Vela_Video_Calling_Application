// group_call — a real cross-laptop call through the SFU.
//
// Connects to an sfu_server (signaling + media hub) over WebSocket, negotiates a
// single ICE channel to it, then:
//   * captures the camera + microphone, encodes (H.264 + Opus), packetizes to
//     RTP, and sends everything up the ICE channel (SendPipeline -> RtpTransport);
//   * receives every other participant's RTP back down the same channel,
//     demultiplexes by SSRC (ReceiveRouter), decodes, and shows one tile per
//     remote video stream while playing all remote audio.
//
// Usage:
//   group_call --server <host|ws://host:port> [--port 8080] --room <name>
//
// Grant the terminal Camera AND Microphone permission (System Settings >
// Privacy & Security). Use headphones — there is no echo cancellation.
//
// THREADING. Signaling callbacks run on the signaling net thread; RtpTransport
// callbacks run on libjuice's thread; capture callbacks run on the capture/audio
// threads. None of those touch Qt. All UI work (self tile, remote tiles) happens
// on the GUI thread: directly in capture->setFrame (VideoWidget is thread-safe)
// and inside the QTimer-driven ReceiveRouter::tick (which fires the tile
// add/frame/remove callbacks on the GUI thread).

#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

#include "audio/AudioIO.h"
#include "capture/CaptureDevice.h"
#include "codec/AudioEncoder.h"
#include "codec/VideoEncoder.h"
#include "common/Clock.h"
#include "media/MediaPipeline.h"
#include "media/ReceiveRouter.h"
#include "signaling/RtpTransport.h"
#include "signaling/SignalingClient.h"
#include "ui/MainWindow.h"
#include "ui/VideoWidget.h"

using namespace vc;

namespace {

struct Args {
    std::string host = "127.0.0.1";
    uint16_t port = 8080;
    std::string room = "demo";
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](std::string& dst) {
            if (i + 1 < argc) dst = argv[++i];
        };
        if (arg == "--server") {
            std::string v;
            next(v);
            // Accept "ws://host:port", "host:port", or "host".
            if (v.rfind("ws://", 0) == 0) v = v.substr(5);
            const auto colon = v.find(':');
            if (colon != std::string::npos) {
                a.host = v.substr(0, colon);
                a.port = static_cast<uint16_t>(std::stoi(v.substr(colon + 1)));
            } else {
                a.host = v;
            }
        } else if (arg == "--port") {
            std::string v;
            next(v);
            if (!v.empty()) a.port = static_cast<uint16_t>(std::stoi(v));
        } else if (arg == "--room") {
            next(a.room);
        }
    }
    return a;
}

uint32_t randomSsrc() {
    std::random_device rd;
    std::mt19937 gen(rd());
    return std::uniform_int_distribution<uint32_t>(1, 0xFFFFFFFEu)(gen);
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);

    // 3.3 core-profile surface so the YUV->RGB shader links (see loopback_call).
    QSurfaceFormat glFmt;
    glFmt.setProfile(QSurfaceFormat::CoreProfile);
    glFmt.setVersion(3, 3);
    glFmt.setDepthBufferSize(0);
    glFmt.setStencilBufferSize(0);
    QSurfaceFormat::setDefaultFormat(glFmt);

    QApplication app(argc, argv);

    ui::MainWindow win;
    win.setWindowTitle(QString("Call — room \"%1\" @ %2:%3")
                           .arg(QString::fromStdString(args.room))
                           .arg(QString::fromStdString(args.host))
                           .arg(args.port));
    win.resize(1100, 720);
    ui::VideoWidget* selfTile = win.addParticipant("self");
    QObject::connect(&win, &ui::MainWindow::leaveRequested, &app, &QApplication::quit);
    win.show();

    // --- Identity + transport ------------------------------------------------
    const uint32_t videoSsrc = randomSsrc();
    const uint32_t audioSsrc = randomSsrc();

    auto transport = std::make_shared<signaling::RtpTransport>();
    auto signaling = std::make_shared<signaling::SignalingClient>();
    std::atomic<bool> iceConnected{false};

    // --- Audio playback for remote streams -----------------------------------
    AudioFormat audioFmt;
    audioFmt.sampleRateHz = 48000;
    audioFmt.channels = 1;
    auto playback = audio::createAudioPlayback();
    if (playback) (void)playback->start(audioFmt);

    // --- Receive side: per-SSRC demux -> decode -> tiles/speaker -------------
    std::unordered_map<uint32_t, ui::VideoWidget*> remoteTiles;  // GUI thread only
    ReceiveRouter::Config rcfg;
    rcfg.targetDelay = std::chrono::milliseconds(80);
    auto receiver = std::make_shared<ReceiveRouter>(
        rcfg, playback.get(),
        /*onVideoFrame=*/[&remoteTiles](uint32_t ssrc, const VideoFrame& f) {
            auto it = remoteTiles.find(ssrc);
            if (it != remoteTiles.end() && it->second) it->second->setFrame(f);
        },
        /*onVideoStreamAdded=*/[&win, &remoteTiles](uint32_t ssrc) {
            const QString id = QString("remote-%1").arg(ssrc);
            if (ui::VideoWidget* tile = win.addParticipant(id)) remoteTiles[ssrc] = tile;
        },
        /*onVideoStreamRemoved=*/[&win, &remoteTiles](uint32_t ssrc) {
            win.removeParticipant(QString("remote-%1").arg(ssrc));
            remoteTiles.erase(ssrc);
        });

    // --- Send side: encoders + pipeline --------------------------------------
    auto venc = std::make_shared<VideoEncoder>();
    auto aenc = std::make_shared<AudioEncoder>();
    (void)aenc->open(audioFmt, 32000);

    SendPipeline::Config scfg;
    scfg.videoSsrc = videoSsrc;
    scfg.audioSsrc = audioSsrc;
    auto sender = std::make_shared<SendPipeline>(
        venc.get(), aenc.get(),
        [transport](const RtpPacket& pkt) { (void)transport->send(pkt); }, scfg);
    std::atomic<bool> videoReady{false};
    std::mutex vencMu;

    // --- Transport callbacks (libjuice thread) -------------------------------
    signaling::RtpTransport::Callbacks tcbs;
    tcbs.onLocalCandidate = [signaling](const std::string& cand) {
        (void)signaling->sendIceCandidate(0, cand);
    };
    tcbs.onPacket = [receiver](const RtpPacket& pkt) { receiver->pushPacket(pkt); };
    tcbs.onStateChanged = [&iceConnected](signaling::IceState s) {
        const bool up = (s == signaling::IceState::Connected ||
                         s == signaling::IceState::Completed);
        iceConnected.store(up);
        std::printf("[ice] %.*s\n", static_cast<int>(to_string(s).size()),
                    to_string(s).data());
        std::fflush(stdout);
    };
    signaling::NATConfig iceCfg;
    iceCfg.stunHost = "";  // LAN: host candidates only
    if (!transport->initialize(iceCfg, std::move(tcbs))) {
        std::fprintf(stderr, "Failed to initialize ICE transport\n");
        return 1;
    }
    (void)transport->startGathering();

    // --- Signaling callbacks (net thread) ------------------------------------
    signaling::SignalingCallbacks scbs;
    scbs.onJoined = [transport, signaling](signaling::PeerId /*self*/) {
        // We are the offerer: hand the server our ICE description.
        auto desc = transport->localDescription();
        if (desc) (void)signaling->sendOffer(0, desc.value());
    };
    scbs.onAnswer = [transport](signaling::PeerId, const std::string& sdp) {
        (void)transport->setRemoteDescription(sdp);
    };
    scbs.onIceCandidate = [transport](signaling::PeerId, const std::string& cand) {
        (void)transport->addRemoteCandidate(cand);
    };
    scbs.onError = [](Error, const std::string& detail) {
        std::fprintf(stderr, "[signaling] error: %s\n", detail.c_str());
    };
    signaling->setCallbacks(std::move(scbs));

    signaling::SignalingConfig sigCfg;
    sigCfg.host = args.host;
    sigCfg.port = args.port;
    sigCfg.room = args.room;
    if (!signaling->connect(sigCfg)) {
        std::fprintf(stderr, "Failed to connect to signaling server at %s:%u\n",
                     args.host.c_str(), args.port);
        return 1;
    }

    // --- Capture: camera + microphone ----------------------------------------
    auto devices = enumerateCaptureDevices();
    if (devices.empty()) {
        std::fprintf(stderr, "No camera found (grant Camera permission).\n");
        return 1;
    }
    auto camera = createCaptureDevice();
    VideoFormat vfmt;
    vfmt.resolution = {640, 480};
    vfmt.fpsNumerator = 30;
    vfmt.fpsDenominator = 1;
    vfmt.pixelFormat = PixelFormat::I420;
    if (!camera->open(devices[0], vfmt)) {
        std::fprintf(stderr, "Failed to open the camera.\n");
        return 1;
    }

    auto onVideo = [&](const VideoFrame& frame) {
        selfTile->setFrame(frame);  // local self-view
        if (win.isCameraOff() || !iceConnected.load()) return;
        if (!videoReady.load(std::memory_order_acquire)) {
            std::lock_guard<std::mutex> lk(vencMu);
            if (!videoReady.load(std::memory_order_relaxed)) {
                VideoEncoder::Config c;
                c.width = frame.width;
                c.height = frame.height;
                c.fps = 30;
                c.bitrateKbps = 1500;
                if (!venc->open(c)) return;
                videoReady.store(true, std::memory_order_release);
            }
        }
        // The capture backend already stamped frame.rtpTimestamp (90 kHz); it is
        // carried through encode -> packetize unchanged.
        (void)sender->pushVideoFrame(frame);
    };
    if (!camera->start(onVideo)) {
        std::fprintf(stderr, "Failed to start the camera.\n");
        return 1;
    }

    auto micCapture = audio::createAudioCapture();
    if (micCapture) {
        auto onAudio = [&](const AudioFrame& frame) {
            if (win.isMuted() || !iceConnected.load()) return;
            (void)sender->pushAudioFrame(frame);
        };
        if (!micCapture->start(audioFmt, onAudio)) {
            std::fprintf(stderr, "Failed to start microphone (check Mic permission).\n");
        }
    }

    // --- Playout pump: drain jitter buffers, decode, render (GUI thread) ------
    QTimer playout;
    QObject::connect(&playout, &QTimer::timeout, [receiver]() {
        receiver->tick(MediaClock::now());
    });
    playout.start(16);

    const int code = app.exec();

    playout.stop();
    (void)camera->stop();
    camera->close();
    if (micCapture) (void)micCapture->stop();
    if (playback) (void)playback->stop();
    signaling->disconnect();
    return code;
}
