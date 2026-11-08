#pragma once

#include "capture/PresentMonCapture.h"
#include "core/CaptureSession.h"

#include <QMainWindow>

#include <vector>

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace framescope::ui {

class FrameTimeView;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

private:
    void buildUi();
    void startCapture();
    void stopCapture();
    void openCsv();
    void browsePresentMon();
    void onFrames(const std::vector<core::FrameRecord>& frames);
    void appendSpikeRow(const core::Spike& spike);
    void rebuildSpikeTable();
    void refresh();
    void log(const QString& message);
    void setCapturing(bool capturing);

    core::CaptureSession m_session;
    capture::PresentMonCapture m_capture;

    FrameTimeView* m_view = nullptr;
    QTableWidget* m_spikeTable = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QLineEdit* m_presentMonPath = nullptr;
    QLineEdit* m_processName = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_openButton = nullptr;
    QLabel* m_statsLabel = nullptr;
    QTimer* m_refreshTimer = nullptr;
    bool m_dirty = false;
};

} // namespace framescope::ui
