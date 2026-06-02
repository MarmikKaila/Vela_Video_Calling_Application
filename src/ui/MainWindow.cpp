#include "ui/MainWindow.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>

#include "ui/VideoWidget.h"

namespace vc::ui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
}

MainWindow::~MainWindow() = default;

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("Video Call"));
    resize(960, 640);

    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    // --- Video grid ---
    gridContainer_ = new QWidget(central);
    grid_ = new QGridLayout(gridContainer_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(6);
    root->addWidget(gridContainer_, /*stretch=*/1);

    // --- Control bar ---
    auto* bar = new QWidget(central);
    auto* barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(0, 0, 0, 0);
    barLayout->addStretch(1);

    muteBtn_ = new QToolButton(bar);
    muteBtn_->setText(QStringLiteral("Mute"));
    muteBtn_->setCheckable(true);
    muteBtn_->setMinimumWidth(90);
    barLayout->addWidget(muteBtn_);

    cameraBtn_ = new QToolButton(bar);
    cameraBtn_->setText(QStringLiteral("Camera Off"));
    cameraBtn_->setCheckable(true);
    cameraBtn_->setMinimumWidth(110);
    barLayout->addWidget(cameraBtn_);

    leaveBtn_ = new QPushButton(QStringLiteral("Leave"), bar);
    leaveBtn_->setMinimumWidth(90);
    barLayout->addWidget(leaveBtn_);

    barLayout->addStretch(1);
    root->addWidget(bar, /*stretch=*/0);

    setCentralWidget(central);

    connect(muteBtn_, &QToolButton::toggled, this, [this](bool checked) {
        muted_ = checked;
        muteBtn_->setText(checked ? QStringLiteral("Unmute")
                                  : QStringLiteral("Mute"));
        emit micToggled(checked);
    });
    connect(cameraBtn_, &QToolButton::toggled, this, [this](bool checked) {
        cameraOff_ = checked;
        cameraBtn_->setText(checked ? QStringLiteral("Camera On")
                                    : QStringLiteral("Camera Off"));
        emit cameraToggled(checked);
    });
    connect(leaveBtn_, &QPushButton::clicked, this,
            [this]() { emit leaveRequested(); });
}

VideoWidget* MainWindow::participant(const QString& id) const {
    auto it = std::find_if(tiles_.begin(), tiles_.end(),
                           [&](const Tile& t) { return t.id == id; });
    return it != tiles_.end() ? it->widget : nullptr;
}

int MainWindow::participantCount() const noexcept {
    return static_cast<int>(tiles_.size());
}

VideoWidget* MainWindow::addParticipant(const QString& id) {
    if (auto* existing = participant(id)) {
        return existing;
    }
    if (static_cast<int>(tiles_.size()) >= kMaxParticipants) {
        return nullptr;
    }
    auto* widget = new VideoWidget(gridContainer_);
    tiles_.push_back(Tile{id, widget});
    relayout();
    return widget;
}

void MainWindow::removeParticipant(const QString& id) {
    auto it = std::find_if(tiles_.begin(), tiles_.end(),
                           [&](const Tile& t) { return t.id == id; });
    if (it == tiles_.end()) {
        return;
    }
    // Detach from the layout and delete the widget.
    grid_->removeWidget(it->widget);
    it->widget->setParent(nullptr);
    it->widget->deleteLater();
    tiles_.erase(it);
    relayout();
}

void MainWindow::relayout() {
    // Remove all widgets from the layout without destroying them.
    for (auto& t : tiles_) {
        grid_->removeWidget(t.widget);
    }

    const int n = static_cast<int>(tiles_.size());
    int rows = 1;
    int cols = 1;
    if (n <= 1) {
        rows = 1;
        cols = 1;
    } else if (n == 2) {
        rows = 1;
        cols = 2;
    } else if (n <= 4) {
        rows = 2;
        cols = 2;
    } else if (n <= 6) {
        rows = 2;
        cols = 3;
    } else { // 7..8
        rows = 2;
        cols = 4;
    }

    // Reset stretch factors then place tiles row-major.
    for (int c = 0; c < 8; ++c) grid_->setColumnStretch(c, 0);
    for (int r = 0; r < 8; ++r) grid_->setRowStretch(r, 0);

    for (int i = 0; i < n; ++i) {
        const int r = i / cols;
        const int c = i % cols;
        grid_->addWidget(tiles_[i].widget, r, c);
    }
    for (int c = 0; c < cols; ++c) grid_->setColumnStretch(c, 1);
    for (int r = 0; r < rows; ++r) grid_->setRowStretch(r, 1);
}

} // namespace vc::ui
