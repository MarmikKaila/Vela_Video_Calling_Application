#pragma once

// VideoWidget — an OpenGL-accelerated renderer for a single I420 VideoFrame.
//
// The widget uploads the Y, U and V planes of an I420 frame as three
// single-channel (GL_RED / GL_R8) textures and performs the YUV->RGB color
// conversion (BT.601 limited-range) inside a GLSL fragment shader. Plane
// strides are respected so frames whose rows are padded for alignment render
// correctly. The video is letterboxed to preserve its source aspect ratio.
//
// Threading: setFrame() is safe to call from any thread (frames may arrive on
// a decode/capture thread). It copies the lightweight VideoFrame value — which
// internally holds a shared_ptr to the pooled backing buffer, so the pixel
// memory stays alive without a deep copy — under a mutex and requests a repaint
// on the GUI thread via a queued update. All OpenGL calls happen on the GUI
// thread inside initializeGL()/paintGL().

#include <QImage>
#include <QOpenGLFunctions>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <memory>
#include <mutex>

#include "common/VideoFrame.h"

QT_FORWARD_DECLARE_CLASS(QOpenGLShaderProgram)

namespace vc::ui {

/// Renders one I420 VideoFrame via an OpenGL YUV->RGB shader, letterboxed.
class VideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    /// Store the latest frame and schedule a repaint. Thread-safe; the frame is
    /// copied by value (shared_ptr-backed, so no pixel copy). A non-I420 or
    /// invalid frame is ignored.
    void setFrame(const VideoFrame& frame);

    /// True once at least one valid frame has been presented.
    [[nodiscard]] bool hasFrame() const;

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    /// Upload one plane into a GL_R8 texture, honoring its row stride.
    void uploadPlane(unsigned texture, const Plane& plane, int width, int height);

    std::unique_ptr<QOpenGLShaderProgram> program_;
    QOpenGLVertexArrayObject vao_; // required for core-profile draw calls
    unsigned vbo_ = 0;
    unsigned textures_[3] = {0, 0, 0}; // Y, U, V

    mutable std::mutex frameMutex_;
    VideoFrame pendingFrame_;      // latest frame awaiting upload (guarded)
    bool hasPending_ = false;      // a new frame is waiting (guarded)
    bool hasEverRendered_ = false; // at least one valid frame uploaded

    int texWidth_ = 0;  // dimensions of currently allocated textures
    int texHeight_ = 0;
    bool glReady_ = false;
};

} // namespace vc::ui
