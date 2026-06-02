// ui_demo — visual proof harness for the UI module.
//
// Default (windowed) mode: opens a MainWindow with four participant tiles, each
// driven by its own MockFrameSource, and logs control-bar signals via qDebug.
//
// Headless mode (`--frames N`): runs without a real display. It creates the
// same window, but instead of entering the event loop indefinitely it pumps N
// frames of generation + rendering through each VideoWidget against whatever
// platform surface Qt provides (use QT_QPA_PLATFORM=offscreen), then exits 0.
// This proves the GL init + upload + draw path works without a monitor.
//
// Usage:
//   ./ui_demo                 # windowed demo
//   QT_QPA_PLATFORM=offscreen ./ui_demo --frames 5
//   ./ui_demo --settings      # also pop the SettingsDialog (windowed only)

#include <QApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QSurfaceFormat>
#include <QTimer>
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "ui/MainWindow.h"
#include "ui/MockFrameSource.h"
#include "ui/SettingsDialog.h"
#include "ui/VideoWidget.h"

namespace {

int parseFramesArg(const QStringList& args) {
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == QStringLiteral("--frames")) {
            if (i + 1 < args.size()) {
                bool ok = false;
                const int n = args[i + 1].toInt(&ok);
                if (ok && n > 0) return n;
            }
            return 5; // sensible default if the count is missing/invalid
        }
    }
    return 0; // 0 => windowed mode
}

} // namespace

int main(int argc, char** argv) {
    // The YUV->RGB shader is GLSL 330 core, so request a 3.3 core-profile
    // surface for every QOpenGLWidget before the application is created.
    QSurfaceFormat fmt;
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setVersion(3, 3);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);

    const QStringList args = app.arguments();
    const int headlessFrames = parseFramesArg(args);
    const bool showSettings = args.contains(QStringLiteral("--settings"));

    vc::ui::MainWindow window;

    // Wire control-bar signals to logging (the "real" app would route these to
    // the audio/capture/session layers).
    QObject::connect(&window, &vc::ui::MainWindow::micToggled,
                     [](bool muted) { qInfo() << "[ui] micToggled muted=" << muted; });
    QObject::connect(&window, &vc::ui::MainWindow::cameraToggled,
                     [](bool off) { qInfo() << "[ui] cameraToggled off=" << off; });
    QObject::connect(&window, &vc::ui::MainWindow::leaveRequested,
                     []() { qInfo() << "[ui] leaveRequested"; });

    // Four participants, each fed by its own mock source.
    std::vector<std::unique_ptr<vc::ui::MockFrameSource>> sources;
    std::array<const char*, 4> ids = {"alice", "bob", "carol", "dave"};
    for (const char* id : ids) {
        auto* tile = window.addParticipant(QString::fromUtf8(id));
        auto src = std::make_unique<vc::ui::MockFrameSource>(640, 360, 30);
        QObject::connect(src.get(), &vc::ui::MockFrameSource::frameReady, tile,
                         [tile](const vc::VideoFrame& f) { tile->setFrame(f); });
        sources.push_back(std::move(src));
    }

    if (headlessFrames > 0) {
        // ----- Headless render loop -----
        window.resize(800, 600);
        window.show(); // realizes GL contexts even on the offscreen platform

        // Process initial show/expose so each QOpenGLWidget initializes its GL.
        app.processEvents();

        std::vector<vc::ui::VideoWidget*> widgets;
        for (const char* id : ids) {
            widgets.push_back(window.participant(QString::fromUtf8(id)));
        }

        int rendered = 0;
        for (int f = 0; f < headlessFrames; ++f) {
            for (std::size_t i = 0; i < sources.size(); ++i) {
                auto r = sources[i]->generateOne();
                if (r) {
                    widgets[i]->setFrame(r.value());
                }
            }
            // Force a synchronous repaint of each widget, then pump events so
            // the deferred paint actually executes on the offscreen surface.
            for (auto* w : widgets) {
                w->update();
            }
            app.processEvents();
            for (auto* w : widgets) {
                w->repaint(); // synchronous paintGL on the offscreen FBO
            }
            ++rendered;
        }

        int withFrame = 0;
        for (auto* w : widgets) {
            if (w->hasFrame()) ++withFrame;
        }
        qInfo() << "[ui] headless rendered" << rendered << "frame(s);"
                << withFrame << "of" << widgets.size()
                << "widgets presented a frame";

        // Success if every widget rendered at least one frame.
        return (withFrame == static_cast<int>(widgets.size())) ? 0 : 2;
    }

    // ----- Windowed demo -----
    for (auto& src : sources) {
        src->start();
    }
    window.show();

    if (showSettings) {
        vc::ui::DeviceList cams = {{"cam0", "FaceTime HD Camera"},
                                   {"cam1", "External USB Camera"}};
        vc::ui::DeviceList mics = {{"mic0", "Built-in Microphone"},
                                   {"mic1", "USB Headset"}};
        auto* dlg = new vc::ui::SettingsDialog(cams, mics, &window);
        QObject::connect(dlg, &vc::ui::SettingsDialog::cameraSelected,
                         [](const std::string& id) {
                             qInfo() << "[ui] cameraSelected"
                                     << QString::fromStdString(id);
                         });
        QObject::connect(dlg, &vc::ui::SettingsDialog::micSelected,
                         [](const std::string& id) {
                             qInfo() << "[ui] micSelected"
                                     << QString::fromStdString(id);
                         });
        dlg->show();
    }

    return app.exec();
}
