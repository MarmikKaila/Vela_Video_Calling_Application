#pragma once

// SettingsDialog — modal device-selection dialog.
//
// Presents two combo boxes (camera and microphone). The device lists are
// INJECTED by the caller — this dialog never touches the capture module. Each
// entry is a (deviceId, humanReadableName) pair; the id is opaque to the UI.
// When the user confirms a selection the dialog emits cameraSelected(id) /
// micSelected(id) and, on accept, exposes the chosen ids via accessors.

#include <QDialog>
#include <QString>
#include <string>
#include <utility>
#include <vector>

// Global-scope forward declaration (see MainWindow.h for why not in-namespace).
QT_FORWARD_DECLARE_CLASS(QComboBox)

namespace vc::ui {

/// Device entry: stable id plus a display name.
using DeviceEntry = std::pair<std::string, std::string>;
using DeviceList = std::vector<DeviceEntry>;

/// Camera/microphone picker populated from caller-supplied device lists.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    /// Construct with device lists pre-populated.
    SettingsDialog(const DeviceList& cameras, const DeviceList& mics,
                   QWidget* parent = nullptr);

    /// Replace the camera options. Preserves the current selection by id if it
    /// is still present, otherwise selects the first entry.
    void setCameras(const DeviceList& cameras);

    /// Replace the microphone options (same selection-preserving behavior).
    void setMicrophones(const DeviceList& mics);

    /// Currently selected ids (empty string if the corresponding list is empty).
    [[nodiscard]] std::string selectedCameraId() const;
    [[nodiscard]] std::string selectedMicId() const;

signals:
    void cameraSelected(const std::string& id);
    void micSelected(const std::string& id);

private:
    void buildUi();
    static void repopulate(class QComboBox* box, const DeviceList& list);

    QComboBox* cameraBox_ = nullptr;
    QComboBox* micBox_ = nullptr;
};

} // namespace vc::ui
