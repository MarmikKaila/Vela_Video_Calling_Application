#include "ui/SettingsDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>
#include <QVariant>

namespace vc::ui {

SettingsDialog::SettingsDialog(QWidget* parent) : QDialog(parent) {
    buildUi();
}

SettingsDialog::SettingsDialog(const DeviceList& cameras, const DeviceList& mics,
                               QWidget* parent)
    : QDialog(parent) {
    buildUi();
    setCameras(cameras);
    setMicrophones(mics);
}

void SettingsDialog::buildUi() {
    setWindowTitle(QStringLiteral("Settings"));
    setModal(true);

    auto* root = new QVBoxLayout(this);
    auto* form = new QFormLayout();

    cameraBox_ = new QComboBox(this);
    micBox_ = new QComboBox(this);
    form->addRow(new QLabel(QStringLiteral("Camera"), this), cameraBox_);
    form->addRow(new QLabel(QStringLiteral("Microphone"), this), micBox_);
    root->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // The device id is stored in each item's userData (QVariant string).
    connect(cameraBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        emit cameraSelected(selectedCameraId());
    });
    connect(micBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        emit micSelected(selectedMicId());
    });
}

void SettingsDialog::repopulate(QComboBox* box, const DeviceList& list) {
    const QString prevId =
        box->currentData().isValid() ? box->currentData().toString() : QString();

    QSignalBlocker block(box); // avoid emitting during the rebuild
    box->clear();
    for (const auto& [id, name] : list) {
        box->addItem(QString::fromStdString(name), QString::fromStdString(id));
    }

    if (!prevId.isEmpty()) {
        const int idx = box->findData(prevId);
        if (idx >= 0) {
            box->setCurrentIndex(idx);
            return;
        }
    }
    if (box->count() > 0) {
        box->setCurrentIndex(0);
    }
}

void SettingsDialog::setCameras(const DeviceList& cameras) {
    repopulate(cameraBox_, cameras);
}

void SettingsDialog::setMicrophones(const DeviceList& mics) {
    repopulate(micBox_, mics);
}

std::string SettingsDialog::selectedCameraId() const {
    const QVariant d = cameraBox_->currentData();
    return d.isValid() ? d.toString().toStdString() : std::string{};
}

std::string SettingsDialog::selectedMicId() const {
    const QVariant d = micBox_->currentData();
    return d.isValid() ? d.toString().toStdString() : std::string{};
}

} // namespace vc::ui
