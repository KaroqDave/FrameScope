#pragma once

#include "capture/PresentMonCapture.h"
#include "core/CaptureSession.h"
#include "core/Comparison.h"
#include "core/Correlator.h"
#include "core/EventTimeline.h"
#include "core/LatencyStats.h"
#include "etw/KernelTrace.h"
#include "win/SystemSampler.h"

#include <QMainWindow>

#include <optional>
#include <vector>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
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
    enum class Mode { Idle, Capture, Baseline };

    void buildUi();
    void buildMenus();

    // Capture
    void startCapture();
    void stopCapture();
    void onCaptureStopped(int exitCode);
    bool startSystemTrace(std::uint32_t targetPid);
    void stopSystemTrace();
    void pumpTrace(bool final);
    void onFrames(const std::vector<core::FrameRecord>& frames);

    // Idle baseline
    void startBaseline();
    void finishBaseline();

    // Files
    void openCsv();
    void openSession();
    void saveSession();
    void exportReport();
    void compareWithEarlierRun();
    void browsePresentMon();
    void resetForLoadedData();

    // Views
    void appendSpikeRow(const core::Spike& spike);
    void rebuildSpikeTable();
    void refresh();
    void updateVerdict();
    void updateFindings();
    [[nodiscard]] std::vector<core::Finding> currentFindings() const;
    [[nodiscard]] std::vector<core::ModuleLatency> currentDrivers() const;
    void logLatencySummary();
    void log(const QString& message);
    void setBusy(bool busy);

    Mode m_mode = Mode::Idle;
    core::CaptureSession m_session;
    core::EventTimeline m_timeline;
    capture::PresentMonCapture m_capture;
    etw::KernelTrace m_trace;
    win::SystemSampler m_sampler;
    core::CorrelatorConfig m_correlatorConfig;

    // Data about the current session
    std::string m_application;
    std::string m_createdAt;
    std::string m_sensors;
    core::LatencyStats m_latency;                   // live capture
    std::optional<std::vector<core::Finding>> m_loadedFindings;  // opened .fscope
    std::vector<core::ModuleLatency> m_loadedDrivers;
    std::vector<core::Finding> m_baselineFindings;
    std::optional<core::RunComparison> m_comparison;

    std::uint32_t m_gamePid = 0;
    bool m_warnedNoDpc = false;
    bool m_loggedSensors = false;
    int m_baselineRemaining = 0;

    FrameTimeView* m_view = nullptr;
    QTableWidget* m_spikeTable = nullptr;
    QPlainTextEdit* m_log = nullptr;
    QListWidget* m_findings = nullptr;
    QLineEdit* m_presentMonPath = nullptr;
    QLineEdit* m_processName = nullptr;
    QCheckBox* m_traceCheck = nullptr;
    QPushButton* m_startButton = nullptr;
    QPushButton* m_stopButton = nullptr;
    QPushButton* m_baselineButton = nullptr;
    QLabel* m_verdictLabel = nullptr;
    QLabel* m_statsLabel = nullptr;
    QTimer* m_refreshTimer = nullptr;
    QTimer* m_traceTimer = nullptr;
    QTimer* m_baselineTimer = nullptr;
    QList<QAction*> m_fileActions; // disabled while busy
    bool m_dirty = false;
};

} // namespace framescope::ui
