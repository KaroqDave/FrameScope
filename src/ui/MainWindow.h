#pragma once

#include "capture/PresentMonCapture.h"
#include "core/CaptureSession.h"
#include "core/EventTimeline.h"
#include "etw/KernelTrace.h"

#include <QMainWindow>

#include <vector>

class QCheckBox;
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
    void onCaptureStopped(int exitCode);
    void openCsv();
    void browsePresentMon();
    void onFrames(const std::vector<core::FrameRecord>& frames);
    void pumpTrace(bool final);
    void appendSpikeRow(const core::Spike& spike);
    void rebuildSpikeTable();
    void refresh();
    void updateVerdict();
    void logLatencySummary();
    void log(const QString& message);
    void setCapturing(bool capturing);

    core::CaptureSession m_session;
    core::EventTimeline m_timeline;
    capture::PresentMonCapture m_capture;
    etw::KernelTrace m_trace;
    bool m_warnedNoDpc = false;

    FrameTimeView* m_view = nullptr;
    QTableWidget* m_spikeTable = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QLineEdit* m_presentMonPath = nullptr;
    QLineEdit* m_processName = nullptr;
    QCheckBox* m_traceCheck = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_openButton = nullptr;
    QLabel* m_verdictLabel = nullptr;
    QLabel* m_statsLabel = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QTimer* m_traceTimer = nullptr;
    bool m_dirty = false;
};

} // namespace framescope::ui
