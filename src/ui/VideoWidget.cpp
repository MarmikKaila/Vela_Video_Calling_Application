#include "ui/VideoWidget.h"

#include <QDebug>
#include <QOpenGLShaderProgram>
#include <QPainter>
#include <algorithm>
#include <array>

namespace vc::ui {

namespace {

// A full-screen triangle pair (two triangles forming a quad) in normalized
// device coordinates, plus matching texture coordinates. The viewport is
// adjusted in paintGL() to letterbox, so the geometry itself stays unit-sized.
// Layout per vertex: x, y, u, v.
constexpr std::array<float, 24> kQuad = {
    // position    // texcoord
    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f, -1.0f, 1.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 0.0f,

    -1.0f, -1.0f, 0.0f, 1.0f,
     1.0f,  1.0f, 1.0f, 0.0f,
    -1.0f,  1.0f, 0.0f, 0.0f,
};

constexpr const char* kVertexShader = R"(
#version 330 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord = aTexCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)";

// BT.601 limited-range YUV -> RGB. Y in [16,235], U/V in [16,240] when sampled
// as 8-bit normalized [0,1]. The constants below are the standard full-matrix
// form: R = 1.164*(Y-16/255) + 1.596*(V-0.5), etc.
constexpr const char* kFragmentShader = R"(
#version 330 core
in vec2 vTexCoord;
out vec4 fragColor;
uniform sampler2D yTex;
uniform sampler2D uTex;
uniform sampler2D vTex;
void main() {
    float y = texture(yTex, vTexCoord).r;
    float u = texture(uTex, vTexCoord).r;
    float v = texture(vTex, vTexCoord).r;
    y = 1.1643 * (y - 0.0625);
    u = u - 0.5;
    v = v - 0.5;
    float r = y + 1.5958 * v;
    float g = y - 0.39173 * u - 0.81290 * v;
    float b = y + 2.017 * u;
    fragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
)";

} // namespace

VideoWidget::VideoWidget(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(160, 90);
}

VideoWidget::~VideoWidget() {
    // Make our GL context current so texture/buffer deletion targets the right
    // context. If there is no valid context (never shown), skip cleanup.
    if (context()) {
        makeCurrent();
        if (vbo_) glDeleteBuffers(1, &vbo_);
        vao_.destroy();
        program_.reset();
        if (textures_[0]) glDeleteTextures(3, textures_);
        doneCurrent();
    }
}

void VideoWidget::setFrame(const VideoFrame& frame) {
    if (!frame.valid() || frame.format != PixelFormat::I420) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        pendingFrame_ = frame; // shared_ptr-backed copy, no pixel duplication
        hasPending_ = true;
    }
    // update() is thread-safe in Qt: it posts a deferred-paint event to the
    // widget's (GUI) thread, so this is safe to call from any thread.
    update();
}

bool VideoWidget::hasFrame() const {
    std::lock_guard<std::mutex> lk(frameMutex_);
    return hasEverRendered_;
}

void VideoWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.05f, 0.05f, 0.07f, 1.0f);

    program_ = std::make_unique<QOpenGLShaderProgram>();
    program_->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertexShader);
    program_->addShaderFromSourceCode(QOpenGLShader::Fragment, kFragmentShader);
    if (!program_->link()) {
        // A link failure here almost always means the GL context is not a 3.3
        // core profile (the GLSL 330 shaders won't compile), which renders the
        // tile blank. Surface the reason instead of silently showing white, and
        // drop the program so paintGL() skips the draw.
        qWarning() << "[VideoWidget] shader link failed (need an OpenGL 3.3 "
                      "core-profile surface):" << program_->log();
        program_.reset();
    }

    program_->bind();
    program_->setUniformValue("yTex", 0);
    program_->setUniformValue("uTex", 1);
    program_->setUniformValue("vTex", 2);
    program_->release();

    // A VAO is mandatory in an OpenGL core profile (the only kind macOS offers)
    // for glDrawArrays to produce output. Record the vertex attribute layout in
    // it once so paintGL only needs to bind the VAO.
    vao_.create();
    vao_.bind();

    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kQuad), kQuad.data(), GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          reinterpret_cast<void*>(2 * sizeof(float)));

    vao_.release();
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    glGenTextures(3, textures_);
    for (unsigned tex : textures_) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    glReady_ = true;
}

void VideoWidget::resizeGL(int /*w*/, int /*h*/) {
    // Viewport is computed per-frame in paintGL() for letterboxing.
}

void VideoWidget::uploadPlane(unsigned texture, const Plane& plane, int width, int height) {
    glBindTexture(GL_TEXTURE_2D, texture);
    // I420 planes are byte-packed; one byte per texel. Match the row stride so
    // padded rows upload correctly. GL_UNPACK_ROW_LENGTH is in texels, which for
    // a single-byte plane equals the stride in bytes.
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, plane.stride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED,
                 GL_UNSIGNED_BYTE, plane.data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

void VideoWidget::paintGL() {
    const qreal dpr = devicePixelRatioF();
    const int fbW = static_cast<int>(width() * dpr);
    const int fbH = static_cast<int>(height() * dpr);

    glViewport(0, 0, fbW, fbH);
    glClear(GL_COLOR_BUFFER_BIT);

    // Pull the latest frame (if any) under lock, then upload outside the lock.
    VideoFrame frame;
    bool haveNew = false;
    {
        std::lock_guard<std::mutex> lk(frameMutex_);
        if (hasPending_) {
            frame = pendingFrame_;
            hasPending_ = false;
            haveNew = true;
        }
    }

    if (haveNew && frame.valid()) {
        const int w = frame.width;
        const int h = frame.height;
        const int cw = (w + 1) / 2;
        const int ch = (h + 1) / 2;

        glActiveTexture(GL_TEXTURE0);
        uploadPlane(textures_[0], frame.planes[0], w, h);
        glActiveTexture(GL_TEXTURE1);
        uploadPlane(textures_[1], frame.planes[1], cw, ch);
        glActiveTexture(GL_TEXTURE2);
        uploadPlane(textures_[2], frame.planes[2], cw, ch);

        texWidth_ = w;
        texHeight_ = h;
        {
            std::lock_guard<std::mutex> lk(frameMutex_);
            hasEverRendered_ = true;
        }
    }

    if (texWidth_ <= 0 || texHeight_ <= 0 || !program_) {
        return; // nothing to draw yet
    }

    // Letterbox: fit the source aspect ratio inside the framebuffer.
    const double srcAspect = static_cast<double>(texWidth_) / texHeight_;
    const double dstAspect = static_cast<double>(fbW) / std::max(1, fbH);
    int vpW = fbW;
    int vpH = fbH;
    if (dstAspect > srcAspect) {
        vpW = static_cast<int>(fbH * srcAspect);
    } else {
        vpH = static_cast<int>(fbW / srcAspect);
    }
    const int vpX = (fbW - vpW) / 2;
    const int vpY = (fbH - vpH) / 2;
    glViewport(vpX, vpY, std::max(1, vpW), std::max(1, vpH));

    program_->bind();
    vao_.bind();

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures_[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, textures_[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, textures_[2]);

    glDrawArrays(GL_TRIANGLES, 0, 6);

    vao_.release();
    program_->release();
}

} // namespace vc::ui
