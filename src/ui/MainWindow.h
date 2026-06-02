#pragma once

// MainWindow — the top-level call window.
//
// It hosts a responsive grid (QGridLayout) of up to 8 VideoWidgets, one per
// participant, and a bottom control bar with Mute, Camera and Leave controls.
// The grid re-flows as participants are added/removed:
//     1 -> 1x1, 2 -> 1x2, 3-4 -> 2x2, 5-6 -> 2x3, 7-8 -> 2x4.
//
// The window owns no media logic. The control buttons translate to the signals
// micToggled(bool), cameraToggled(bool) and leaveRequested(); callers wire
// those to the real audio/capture/session layers. Participant tiles are keyed
// by an opaque string id supplied by the caller.

#include <QHash>
#include <QMainWindow>
#include <QString>
#include <vector>

// Forward-declare Qt classes at GLOBAL scope. Declaring them inside namespace
// vc::ui (e.g. `class QGridLayout* grid_;`) would create phantom vc::ui::QGridLayout
// types distinct from Qt's, yielding "incomplete type" errors in the .cpp.
QT_FORWARD_DECLARE_CLASS(QGridLayout)
QT_FORWARD_DECLARE_CLASS(QToolButton)
QT_FORWARD_DECLARE_CLASS(QPushButton)

namespace vc::ui {

class VideoWidget;

/// Top-level call window: participant video grid plus a control bar.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    /// Add a participant tile and return its VideoWidget (owned by the window).
    /// If `id` already exists the existing widget is returned. Returns nullptr
    /// if the grid is full (8 participants).
    VideoWidget* addParticipant(const QString& id);

    /// Remove the participant's tile and re-flow the grid. No-op if unknown.
    void removeParticipant(const QString& id);

    /// Look up an existing tile, or nullptr.
    [[nodiscard]] VideoWidget* participant(const QString& id) const;

    [[nodiscard]] int participantCount() const noexcept;

    /// Current mute / camera-off UI state (reflects the toggle buttons).
    [[nodiscard]] bool isMuted() const noexcept { return muted_; }
    [[nodiscard]] bool isCameraOff() const noexcept { return cameraOff_; }

signals:
    void micToggled(bool muted);       // true => microphone muted
    void cameraToggled(bool cameraOff); // true => camera disabled
    void leaveRequested();

private:
    void buildUi();
    void relayout();

    static constexpr int kMaxParticipants = 8;

    struct Tile {
        QString id;
        VideoWidget* widget = nullptr;
    };

    QWidget* gridContainer_ = nullptr;
    QGridLayout* grid_ = nullptr;
    std::vector<Tile> tiles_; // insertion order drives grid placement

    QToolButton* muteBtn_ = nullptr;
    QToolButton* cameraBtn_ = nullptr;
    QPushButton* leaveBtn_ = nullptr;

    bool muted_ = false;
    bool cameraOff_ = false;
};

} // namespace vc::ui
