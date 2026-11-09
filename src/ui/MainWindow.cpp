#include "ui/MainWindow.h"

#include "core/PresentMonCsvParser.h"
#include "core/Report.h"
#include "core/SessionFile.h"
#include "ui/FrameTimeView.h"
#include "ui/Settings.h"

#include <QAction>
#include <QCoreApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QSysInfo>
#include <QTextStream>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <exception>
#include <map>

namespace framescope::ui {
namespace {

constexpr auto kKeyPresentMon = "capture/presentMonPath";
constexpr auto kKeyProcess = "capture/processName";
constexpr auto kKeyTrace = "capture/systemTrace";
constexpr auto kKeyLastDir = "files/lastDir";

constexpr int kBaselineSeconds = 30;

// Spikes are attributed once the trace has covered them. CPU buckets lag
// ~2 s behind (per-CPU ETW buffers), so wait a bit longer than that.
constexpr double kSettleMs = 4000.0;

enum SpikeColumn { ColTime, ColFrame, ColExcess, ColBound, ColSide, ColCause, ColConfidence, ColDetail, ColCount };

QTableWidgetItem* numberItem(double value, int decimals)
{
    auto* item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, QString::number(value, 'f', decimals));
    item->setData(Qt::UserRole, value);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

QTableWidgetItem* textItem(std::string_view text)
{
    return new QTableWidgetItem(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
}

QString qs(const std::string& s)
{
    return QString::fromStdString(s);
}

QColor findingColor(core::Finding::Level level)
{
    switch (level) {
    case core::Finding::Level::Good:    return {0x43, 0xa0, 0x47};
    case core::Finding::Level::Info:    return {0x60, 0x7d, 0x8b};
    case core::Finding::Level::Warning: return {0xef, 0x8f, 0x00};
    case core::Finding::Level::Problem: return {0xe5, 0x39, 0x35};
    }
    return {};
}

QColor sideColor(core::Side side)
{
    switch (side) {
    case core::Side::System:  return {0xff, 0x52, 0x52, 70};
    case core::Side::Game:    return {0x7c, 0x83, 0xff, 70};
    case core::Side::Mixed:   return {0xff, 0xb3, 0x00, 70};
    case core::Side::Unknown: break;
    }
    return {};
}

QString lastDir()
{
    return QSettings(settingsFile(), QSettings::IniFormat).value(kKeyLastDir).toString();
}

void rememberDir(const QString& file)
{
    QSettings(settingsFile(), QSettings::IniFormat).setValue(kKeyLastDir, QFileInfo(file).absolutePath());
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_timeline(etw::KernelTrace::qpcFrequency())
{
    m_correlatorConfig.processName = [](std::uint32_t pid) { return win::processName(pid); };
    buildUi();
    buildMenus();

    QSettings settings(settingsFile(), QSettings::IniFormat);
    m_presentMonPath->setText(settings.value(kKeyPresentMon).toString());
    m_processName->setText(settings.value(kKeyProcess).toString());
    m_traceCheck->setChecked(etw::KernelTrace::isSupported() && settings.value(kKeyTrace, true).toBool());

    connect(&m_capture, &capture::PresentMonCapture::framesCaptured, this, &MainWindow::onFrames);
    connect(&m_capture, &capture::PresentMonCapture::logMessage, this, &MainWindow::log);
    connect(&m_capture, &capture::PresentMonCapture::started, this, [this] {
        log(tr("Capture started. Waiting for frames from %1 …").arg(m_processName->text()));
    });
    connect(&m_capture, &capture::PresentMonCapture::stopped, this, &MainWindow::onCaptureStopped);
    connect(&m_capture, &capture::PresentMonCapture::failed, this, [this](const QString& msg) {
        log(msg);
        stopSystemTrace();
        m_mode = Mode::Idle;
        setBusy(false);
        QMessageBox::warning(this, tr("Capture failed"), msg);
    });

    // Repaint at ~30 Hz instead of per frame batch.
    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(33);
    connect(m_refreshTimer, &QTimer::timeout, this, [this] {
        if (m_dirty)
            refresh();
    });
    m_refreshTimer->start();

    // Pull ETW events/samples and attribute settled spikes once a second.
    m_traceTimer = new QTimer(this);
    m_traceTimer->setInterval(1000);
    connect(m_traceTimer, &QTimer::timeout, this, [this] { pumpTrace(false); });

    m_baselineTimer = new QTimer(this);
    m_baselineTimer->setInterval(1000);
    connect(m_baselineTimer, &QTimer::timeout, this, [this] {
        if (--m_baselineRemaining <= 0)
            finishBaseline();
        else
            m_verdictLabel->setText(tr("Idle baseline: %1 s left. Keep the PC idle, don't start the game.")
                                        .arg(m_baselineRemaining));
    });

    setBusy(false);
    updateFindings();
    refresh();
}

MainWindow::~MainWindow()
{
    QSettings settings(settingsFile(), QSettings::IniFormat);
    settings.setValue(kKeyPresentMon, m_presentMonPath->text());
    settings.setValue(kKeyProcess, m_processName->text());
    settings.setValue(kKeyTrace, m_traceCheck->isChecked());
    m_capture.stop();
    m_trace.stop();
    m_sampler.stop();
}

// ===========================================================================
// UI
// ===========================================================================

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("FrameScope %1").arg(QCoreApplication::applicationVersion()));
    resize(1280, 820);

    m_presentMonPath = new QLineEdit;
    m_presentMonPath->setPlaceholderText(tr("Path to PresentMon-2.x-x64.exe"));
    auto* browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::browsePresentMon);

    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_presentMonPath, 1);
    pathRow->addWidget(browse);

    m_processName = new QLineEdit;
    m_processName->setPlaceholderText(tr("Game executable, e.g. cod.exe"));

    const bool traceSupported = etw::KernelTrace::isSupported();
    m_traceCheck = new QCheckBox(tr("System trace"));
    m_traceCheck->setToolTip(traceSupported
                                 ? tr("Record driver latency, page faults, disk I/O, CPU scheduling, "
                                      "RAM/VRAM and GPU sensors to find out what caused each spike.")
                                 : tr("Only available on Windows."));

    m_startButton = new QPushButton(tr("Start capture"));
    m_stopButton = new QPushButton(tr("Stop"));
    m_baselineButton = new QPushButton(tr("Idle baseline"));
    m_baselineButton->setToolTip(tr("%1 s system check without the game: driver latency, background load, "
                                    "RAM and GPU state at idle.").arg(kBaselineSeconds));
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startCapture);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopCapture);
    connect(m_baselineButton, &QPushButton::clicked, this, &MainWindow::startBaseline);
    connect(m_processName, &QLineEdit::returnPressed, this, &MainWindow::startCapture);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_processName, 1);
    buttonRow->addWidget(m_traceCheck);
    buttonRow->addWidget(m_startButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addSpacing(16);
    buttonRow->addWidget(m_baselineButton);

    auto* form = new QFormLayout;
    form->addRow(tr("PresentMon:"), pathRow);
    form->addRow(tr("Process:"), buttonRow);

    m_verdictLabel = new QLabel;
    m_verdictLabel->setWordWrap(true);
    QFont vf = m_verdictLabel->font();
    vf.setPointSizeF(vf.pointSizeF() * 1.15);
    vf.setBold(true);
    m_verdictLabel->setFont(vf);

    m_view = new FrameTimeView;
    m_view->setSession(&m_session);

    m_spikeTable = new QTableWidget(0, ColCount);
    m_spikeTable->setHorizontalHeaderLabels({tr("Time (s)"), tr("Frame (ms)"), tr("Excess (ms)"), tr("Bound"),
                                             tr("Side"), tr("Cause"), tr("Conf."), tr("Evidence")});
    auto* header = m_spikeTable->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ColDetail, QHeaderView::Stretch);
    m_spikeTable->verticalHeader()->setVisible(false);
    m_spikeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_spikeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_spikeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_spikeTable->setSortingEnabled(true);
    m_spikeTable->setToolTip(tr("Bound: which side was busy in the spike frame.\n"
                                "Side/Cause appear ~4 s after a spike, once the system trace has covered it."));
    connect(m_spikeTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = m_spikeTable->selectionModel()->selectedRows(ColTime);
        if (!rows.isEmpty())
            m_view->focusTime(rows.first().data(Qt::UserRole).toDouble());
    });

    m_findings = new QListWidget;
    m_findings->setWordWrap(true);
    m_findings->setSelectionMode(QAbstractItemView::NoSelection);

    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);

    auto* tabs = new QTabWidget;
    tabs->addTab(m_findings, tr("Findings"));
    tabs->addTab(m_log, tr("Log"));

    auto* bottom = new QSplitter(Qt::Horizontal);
    bottom->addWidget(m_spikeTable);
    bottom->addWidget(tabs);
    bottom->setStretchFactor(0, 3);
    bottom->setStretchFactor(1, 2);

    auto* main = new QSplitter(Qt::Vertical);
    main->addWidget(m_view);
    main->addWidget(bottom);
    main->setStretchFactor(0, 3);
    main->setStretchFactor(1, 2);

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->addLayout(form);
    layout->addWidget(m_verdictLabel);
    layout->addWidget(main, 1);
    setCentralWidget(central);

    m_statsLabel = new QLabel;
    statusBar()->addPermanentWidget(m_statsLabel, 1);
}

void MainWindow::buildMenus()
{
    auto* file = menuBar()->addMenu(tr("&File"));
    auto add = [&](QMenu* menu, const QString& text, auto slot, QKeySequence key = {}) {
        auto* a = menu->addAction(text, this, slot);
        if (!key.isEmpty())
            a->setShortcut(key);
        m_fileActions << a;
        return a;
    };
    add(file, tr("Open PresentMon CSV…"), &MainWindow::openCsv);
    add(file, tr("&Open session…"), &MainWindow::openSession, QKeySequence::Open);
    add(file, tr("&Save session…"), &MainWindow::saveSession, QKeySequence::Save);
    file->addSeparator();
    add(file, tr("&Export report…"), &MainWindow::exportReport, QKeySequence(tr("Ctrl+E")));
    file->addSeparator();
    auto* quit = file->addAction(tr("&Quit"), this, &QWidget::close);
    quit->setShortcut(QKeySequence::Quit);

    auto* tools = menuBar()->addMenu(tr("&Tools"));
    add(tools, tr("Idle &baseline (%1 s)").arg(kBaselineSeconds), &MainWindow::startBaseline);
    add(tools, tr("&Compare with earlier run…"), &MainWindow::compareWithEarlierRun);
    tools->addSeparator();
    // Stays enabled while capturing: a snapshot mid-capture is useful too.
    tools->addAction(tr("Save &diagnostics…"), this, &MainWindow::saveDiagnostics);
}

void MainWindow::setBusy(bool busy)
{
    const bool supported = etw::KernelTrace::isSupported();
    m_startButton->setEnabled(!busy);
    m_stopButton->setEnabled(busy);
    m_baselineButton->setEnabled(!busy && supported);
    m_presentMonPath->setEnabled(!busy);
    m_processName->setEnabled(!busy);
    m_traceCheck->setEnabled(!busy && supported);
    for (auto* a : m_fileActions)
        a->setEnabled(!busy);
}

// ===========================================================================
// System trace (shared by capture and baseline)
// ===========================================================================

bool MainWindow::startSystemTrace(std::uint32_t targetPid)
{
    const auto topo = win::queryCpuTopology();
    m_timeline.setTopology(topo);
    if (topo.hasAsymmetricCache() && m_mode == Mode::Capture)
        log(tr("CPU has 3D V-Cache on CCD%1: checking where the game's threads run.").arg(topo.vcacheCcd));

    etw::KernelTraceOptions opts;
    opts.ccdOfCpu = topo.ccdOfCpu;
    m_sampler.setTargetProcess(targetPid);
    m_sampler.start(250);

    if (!m_trace.start(opts)) {
        log(tr("System trace unavailable: %1").arg(qs(m_trace.lastError())));
        m_sampler.stop();
        return false;
    }
    m_trace.setTargetProcess(targetPid);
    m_timeline.setTraceStart(m_trace.startQpc());
    m_warnedNoDpc = false;
    m_loggedSensors = false;
    log(tr("System trace running (%1).").arg(qs(m_trace.sessionDescription())));
    m_traceTimer->start();
    return true;
}

void MainWindow::stopSystemTrace()
{
    m_traceTimer->stop();
    m_trace.stop();   // flushes the last CPU buckets
    m_sampler.stop();
}

void MainWindow::pumpTrace(bool final)
{
    const auto events = m_trace.drainEvents();
    const auto buckets = m_trace.drainCpuBuckets();
    const auto samples = m_sampler.drain();
    m_timeline.append(events);
    m_timeline.appendCpuBuckets(buckets);
    m_timeline.appendSamples(samples);
    m_timeline.noteCoverage(final ? etw::KernelTrace::qpcNow() : m_trace.lastEventQpc());

    if (!m_loggedSensors && !samples.empty()) {
        m_loggedSensors = true;
        m_sensors = m_sampler.status();
        log(tr("Sensors: %1").arg(qs(m_sensors)));
    }

    if (m_mode == Mode::Capture) {
        m_latency = m_trace.latencySnapshot();
        const double settleMs = final ? 0.0 : kSettleMs;
        if (m_session.attributePending(m_timeline, settleMs, m_correlatorConfig) > 0) {
            rebuildSpikeTable();
            updateVerdict();
            m_view->update();
        }
        // CPU buckets are only needed until their spikes are attributed.
        if (!final)
            m_timeline.pruneBucketsBefore(m_timeline.coveredUntil() - m_timeline.fromMilliseconds(30'000.0));
        updateFindings();
    }

    // A session that starts fine but delivers nothing is the most likely
    // failure mode on unusual systems; say so instead of failing silently.
    const auto runningFor = m_timeline.toMicroseconds(etw::KernelTrace::qpcNow() - m_trace.startQpc());
    if (!m_warnedNoDpc && !final && runningFor > 3e6 && m_trace.dpcIsrCount() == 0) {
        m_warnedNoDpc = true;
        log(tr("Warning: the system trace delivers no DPC/ISR events. "
               "Run framescope-latency --nt-kernel-logger to check."));
    }
}

// ===========================================================================
// Capture
// ===========================================================================

void MainWindow::startCapture()
{
    if (m_mode != Mode::Idle)
        return;

    m_mode = Mode::Capture;
    m_session.clear();
    m_timeline.clear();
    m_latency.clear();
    m_loadedFindings.reset();
    m_loadedDrivers.clear();
    m_comparison.reset();
    m_application = m_processName->text().trimmed().toStdString();
    m_createdAt = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();
    m_sensors.clear();
    m_gamePid = 0;
    m_spikeTable->setRowCount(0);
    m_view->setFollow(true);
    m_view->setSpanSeconds(30.0);
    setBusy(true);

    // Start the system trace first so it covers the very first frames.
    if (m_traceCheck->isChecked())
        startSystemTrace(0); // the game's PID follows with the first frames

    m_capture.start({
        .presentMonPath = m_presentMonPath->text().trimmed(),
        .processName = m_processName->text().trimmed(),
        .extraArgs = {},
        .qpcFrequency = etw::KernelTrace::qpcFrequency(),
    });
    updateVerdict();
    updateFindings();
}

void MainWindow::stopCapture()
{
    if (m_mode == Mode::Baseline) {
        finishBaseline();
        return;
    }
    log(tr("Stopping capture …"));
    m_capture.stop(); // emits stopped() -> onCaptureStopped()
}

void MainWindow::onCaptureStopped(int exitCode)
{
    log(tr("PresentMon exited (code %1).").arg(exitCode));
    if (m_trace.isRunning() || m_traceTimer->isActive()) {
        stopSystemTrace();
        pumpTrace(true);
        logLatencySummary();
    }
    m_mode = Mode::Idle;
    setBusy(false);
    updateVerdict();
    updateFindings();
    refresh();
}

void MainWindow::onFrames(const std::vector<core::FrameRecord>& frames)
{
    if (m_gamePid == 0 && !frames.empty() && frames.front().processId != 0) {
        m_gamePid = frames.front().processId;
        if (!frames.front().application.empty())
            m_application = frames.front().application;
        m_trace.setTargetProcess(m_gamePid);
        m_sampler.setTargetProcess(m_gamePid);
        log(tr("Game process: %1 (PID %2)").arg(qs(m_application)).arg(m_gamePid));
    }

    // Sorting while inserting would move rows around; pause it.
    m_spikeTable->setSortingEnabled(false);
    for (const auto& f : frames) {
        if (auto spike = m_session.append(f))
            appendSpikeRow(*spike);
    }
    m_spikeTable->setSortingEnabled(true);
    m_dirty = true;
}

// ===========================================================================
// Idle baseline
// ===========================================================================

void MainWindow::startBaseline()
{
    if (m_mode != Mode::Idle || !etw::KernelTrace::isSupported())
        return;

    m_mode = Mode::Baseline;
    m_timeline.clear();
    setBusy(true);
    log(tr("Idle baseline started (%1 s).").arg(kBaselineSeconds));

    if (!startSystemTrace(0)) {
        m_mode = Mode::Idle;
        setBusy(false);
        QMessageBox::warning(this, tr("Baseline failed"), qs(m_trace.lastError()));
        return;
    }
    m_baselineRemaining = kBaselineSeconds;
    m_verdictLabel->setText(tr("Idle baseline: %1 s left. Keep the PC idle, don't start the game.")
                                .arg(m_baselineRemaining));
    m_baselineTimer->start();
}

void MainWindow::finishBaseline()
{
    m_baselineTimer->stop();
    stopSystemTrace();
    pumpTrace(true);

    const auto latency = m_trace.latencySnapshot();
    m_baselineFindings = core::sessionFindings(m_timeline, latency.empty() ? nullptr : &latency, m_correlatorConfig);
    m_timeline.clear(); // baseline data must not leak into the next capture's checks

    int problems = 0, warnings = 0;
    for (const auto& f : m_baselineFindings) {
        problems += f.level == core::Finding::Level::Problem;
        warnings += f.level == core::Finding::Level::Warning;
    }
    log(tr("Idle baseline finished: %1 problems, %2 warnings.").arg(problems).arg(warnings));

    m_mode = Mode::Idle;
    setBusy(false);
    updateFindings();
    m_verdictLabel->setText(problems > 0 ? tr("Idle baseline: %1 problem(s) even without the game, see Findings.").arg(problems)
                            : warnings > 0 ? tr("Idle baseline: %1 warning(s), see Findings.").arg(warnings)
                                           : tr("Idle baseline: the system looks clean at idle."));
}

// ===========================================================================
// Files
// ===========================================================================

void MainWindow::browsePresentMon()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Select PresentMon"), m_presentMonPath->text(),
                                                   tr("Executables (*.exe);;All files (*)"));
    if (!path.isEmpty())
        m_presentMonPath->setText(path);
}

void MainWindow::resetForLoadedData()
{
    m_timeline.clear();
    m_latency.clear();
    m_loadedFindings.reset();
    m_loadedDrivers.clear();
    m_comparison.reset();
    m_sensors.clear();
    m_createdAt.clear();
    m_application.clear();
}

void MainWindow::openCsv()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Open PresentMon CSV"), lastDir(),
                                                   tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;
    rememberDir(path);

    try {
        auto frames = core::loadPresentMonCsv(path.toStdWString());
        resetForLoadedData();
        m_session.clear();
        for (auto& f : frames)
            m_session.append(std::move(f));
        if (!m_session.frames().empty())
            m_application = m_session.frames().front().application;
        m_createdAt = QFileInfo(path).lastModified().toString(Qt::ISODate).toStdString();
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Could not open CSV"), QString::fromUtf8(e.what()));
        return;
    }

    rebuildSpikeTable();
    m_view->showAll();
    log(tr("Loaded %1 frames from %2 (no system trace: causes need a live capture)")
            .arg(m_session.frames().size())
            .arg(path));
    updateVerdict();
    updateFindings();
    refresh();
}

void MainWindow::openSession()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Open session"), lastDir(),
                                                   tr("FrameScope sessions (*.fscope);;All files (*)"));
    if (path.isEmpty())
        return;
    rememberDir(path);

    try {
        auto s = core::loadSession(path.toStdWString());
        resetForLoadedData();
        m_application = s.application;
        m_createdAt = s.createdAt;
        m_sensors = s.sensors;
        m_loadedFindings = std::move(s.findings);
        m_loadedDrivers = std::move(s.drivers);
        m_session.restore(std::move(s.frames), std::move(s.spikes));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Could not open session"), QString::fromUtf8(e.what()));
        return;
    }

    rebuildSpikeTable();
    m_view->showAll();
    log(tr("Opened session %1 (%2 frames, %3 spikes)").arg(path).arg(m_session.frames().size()).arg(m_session.spikes().size()));
    updateVerdict();
    updateFindings();
    refresh();
}

void MainWindow::saveSession()
{
    if (m_session.frames().empty()) {
        QMessageBox::information(this, tr("Save session"), tr("Nothing to save yet."));
        return;
    }
    const auto suggested = QStringLiteral("%1/%2-%3.fscope")
                               .arg(lastDir(), QFileInfo(qs(m_application)).completeBaseName().isEmpty()
                                                   ? QStringLiteral("capture")
                                                   : QFileInfo(qs(m_application)).completeBaseName(),
                                    QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm")));
    const auto path = QFileDialog::getSaveFileName(this, tr("Save session"), suggested,
                                                   tr("FrameScope sessions (*.fscope)"));
    if (path.isEmpty())
        return;
    rememberDir(path);

    core::SavedSession s;
    s.application = m_application;
    s.createdAt = m_createdAt;
    s.sensors = m_sensors;
    s.frames = m_session.frames();
    s.spikes = m_session.spikes();
    s.findings = currentFindings();
    s.drivers = currentDrivers();
    s.gamePid = m_gamePid;
    s.shaderWrites = core::shaderWritesForSession(m_timeline, m_session.frames());
    try {
        core::saveSession(path.toStdWString(), s);
        log(tr("Saved session to %1").arg(path));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Could not save session"), QString::fromUtf8(e.what()));
    }
}

void MainWindow::exportReport()
{
    if (m_session.frames().empty() && m_baselineFindings.empty()) {
        QMessageBox::information(this, tr("Export report"), tr("Nothing to report yet."));
        return;
    }
    const auto path = QFileDialog::getSaveFileName(this, tr("Export report"),
                                                   lastDir() + QStringLiteral("/framescope-report.html"),
                                                   tr("HTML files (*.html)"));
    if (path.isEmpty())
        return;
    rememberDir(path);

    core::ReportData d;
    d.title = m_application.empty() ? "FrameScope report" : "FrameScope report: " + m_application;
    d.application = m_application;
    d.createdAt = m_createdAt;
    d.sensors = m_sensors;
    d.frames = m_session.frames();
    d.spikes = m_session.spikes();
    d.findings = currentFindings();
    d.drivers = currentDrivers();
    d.baseline = m_baselineFindings;
    d.comparison = m_comparison;
    const auto html = core::renderHtmlReport(d);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(html.data(), static_cast<qint64>(html.size())) != static_cast<qint64>(html.size())) {
        QMessageBox::warning(this, tr("Could not export report"), file.errorString());
        return;
    }
    file.close();
    log(tr("Exported report to %1").arg(path));
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::compareWithEarlierRun()
{
    if (m_session.frames().empty()) {
        QMessageBox::information(this, tr("Compare runs"),
                                 tr("Capture or open the second run first, then pick the earlier run to compare with."));
        return;
    }
    const auto path = QFileDialog::getOpenFileName(this, tr("Pick the earlier run (run 1)"), lastDir(),
                                                   tr("Sessions and captures (*.fscope *.csv);;All files (*)"));
    if (path.isEmpty())
        return;
    rememberDir(path);

    core::CaptureSession earlier;
    try {
        if (core::isSessionFile(path.toStdWString())) {
            auto s = core::loadSession(path.toStdWString());
            earlier.restore(std::move(s.frames), std::move(s.spikes));
        } else {
            for (auto& f : core::loadPresentMonCsv(path.toStdWString()))
                earlier.append(std::move(f));
        }
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Could not open run"), QString::fromUtf8(e.what()));
        return;
    }

    m_comparison = core::compareRuns(earlier.frames(), earlier.spikes(), m_session.frames(), m_session.spikes());
    log(tr("Compared with %1: %2").arg(QFileInfo(path).fileName(), qs(m_comparison->verdict)));
    updateFindings();
}

void MainWindow::saveDiagnostics()
{
    const auto path = QFileDialog::getSaveFileName(
        this, tr("Save diagnostics"),
        QStringLiteral("%1/framescope-diagnostics-%2.txt").arg(lastDir(), QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmm"))),
        tr("Text files (*.txt)"));
    if (path.isEmpty())
        return;
    rememberDir(path);

    QString text;
    QTextStream o(&text);
    const auto section = [&o](const QString& title) { o << "\n== " << title << " ==\n"; };

    o << "FrameScope diagnostics, " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    section(tr("Build and system"));
    o << "FrameScope   " << QCoreApplication::applicationVersion() << "\n";
    o << "Qt           " << qVersion() << "\n";
#if defined(_MSC_VER)
    o << "Compiler     MSVC " << _MSC_VER << "\n";
#elif defined(__clang__)
    o << "Compiler     Clang " << __clang_major__ << "." << __clang_minor__ << "\n";
#elif defined(__GNUC__)
    o << "Compiler     GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "\n";
#endif
#ifdef FRAMESCOPE_HAVE_ADLX_BUILD
    o << "ADLX         built in\n";
#endif
    o << "OS           " << QSysInfo::prettyProductName() << " (kernel " << QSysInfo::kernelVersion() << ")\n";
    o << "Admin rights " << (win::isElevated() ? "yes" : "no") << "\n";
    o << "CPU          " << qs(core::describe(win::queryCpuTopology())) << "\n";
    o << "QPC freq     " << etw::KernelTrace::qpcFrequency() << " Hz\n";
    o << "Settings     " << settingsFile() << "\n";
    o << "Mode         " << (m_mode == Mode::Capture ? "capturing" : m_mode == Mode::Baseline ? "baseline" : "idle") << "\n";

    section(tr("Sensors"));
    o << (m_sensors.empty() ? qs(m_sampler.status()) : qs(m_sensors)) << "\n";
    const auto& samples = m_timeline.samples();
    const std::size_t first = samples.size() > 10 ? samples.size() - 10 : 0;
    o << samples.size() << " samples on the timeline; last " << (samples.size() - first) << ":\n";
    for (std::size_t i = first; i < samples.size(); ++i)
        o << "  " << qs(core::describe(samples[i])) << "\n";

    section(tr("System trace"));
    o << "Session      " << qs(m_trace.sessionDescription().empty() ? std::string{"(not started)"} : m_trace.sessionDescription()) << "\n";
    if (const auto err = m_trace.lastError(); !err.empty())
        o << "Last error   " << qs(err) << "\n";
    o << qs(etw::describe(m_trace.diagnostics()));
    o << "Timeline     " << m_timeline.size() << " events, game CPU time by CCD:";
    for (std::size_t i = 0; i < m_timeline.topology().ccdCount(); ++i)
        o << " CCD" << i << "=" << qRound(m_timeline.gameUsByCcdTotal()[i] / 1000.0) << " ms";
    o << ", " << m_timeline.gameCrossCcdMigrationsTotal() << " cross-CCD migrations\n";

    section(tr("PresentMon"));
    const auto& parser = m_capture.parser();
    o << "Command      " << (m_capture.commandLine().isEmpty() ? tr("(not started)") : m_capture.commandLine()) << "\n";
    o << "Header       " << qs(parser.headerLine().empty() ? std::string{"(none yet)"} : parser.headerLine()) << "\n";
    o << "QPC times    " << (parser.hasQpcTimes() ? "yes" : "no (spikes can't be matched to system events)") << "\n";
    o << "Swap chain   " << qs(parser.lockedSwapChain()) << "\n";
    o << "Skipped rows " << parser.skippedRows() << "\n";
    o << "Game         " << qs(m_application) << " (PID " << m_gamePid << ")\n";

    section(tr("Session"));
    const auto st = m_session.stats();
    o << st.frames << " frames, " << st.durationSeconds << " s, avg " << st.averageFps << " fps, " << st.spikes << " spikes\n";
    std::map<std::string, int> causes;
    int pending = 0;
    for (const auto& sp : m_session.spikes()) {
        if (sp.attribution)
            ++causes[std::string{core::toString(sp.attribution->cause)}];
        else
            ++pending;
    }
    for (const auto& [cause, n] : causes)
        o << "  " << qs(cause) << ": " << n << "\n";
    if (pending > 0)
        o << "  (not attributed yet: " << pending << ")\n";
    o << "Verdict      " << qs(m_session.diagnosis().verdict) << "\n";

    section(tr("Findings"));
    for (const auto& f : currentFindings())
        o << "[" << QString::fromUtf8(core::toString(f.level).data()) << "] " << qs(f.title) << ": " << qs(f.detail) << "\n";
    for (const auto& f : m_baselineFindings)
        o << "[baseline " << QString::fromUtf8(core::toString(f.level).data()) << "] " << qs(f.title) << ": " << qs(f.detail) << "\n";

    section(tr("Worst drivers"));
    for (const auto& d : currentDrivers())
        o << qs(d.module) << ": max DPC " << d.maxDpcUs / 1000.0 << " ms, max ISR " << d.maxIsrUs / 1000.0
          << " ms, " << d.dpcCount << " DPCs, " << d.over1ms << "x over 1 ms\n";

    section(tr("Log"));
    o << m_log->toPlainText() << "\n";
    o.flush();

    QFile file(path);
    const auto bytes = text.toUtf8();
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) || file.write(bytes) != bytes.size()) {
        QMessageBox::warning(this, tr("Could not save diagnostics"), file.errorString());
        return;
    }
    log(tr("Saved diagnostics to %1").arg(path));
}

// ===========================================================================
// Views
// ===========================================================================

std::vector<core::Finding> MainWindow::currentFindings() const
{
    auto findings = m_loadedFindings ? *m_loadedFindings
                                     : core::sessionFindings(m_timeline, m_latency.empty() ? nullptr : &m_latency,
                                                             m_correlatorConfig);
    // The stutter pattern is recomputed from the frames (also for older saved sessions).
    std::erase_if(findings, [](const core::Finding& f) { return f.title.starts_with("Stutter "); });
    const auto pattern = core::stutterPatternFindings(m_session.frames(), m_session.spikes());
    findings.insert(findings.end(), pattern.begin(), pattern.end());
    return findings;
}

std::vector<core::ModuleLatency> MainWindow::currentDrivers() const
{
    return m_loadedFindings ? m_loadedDrivers : m_latency.worst(15);
}

void MainWindow::updateFindings()
{
    m_findings->clear();
    const auto header = [this](const QString& text) {
        auto* item = new QListWidgetItem(text);
        QFont f = item->font();
        f.setBold(true);
        item->setFont(f);
        m_findings->addItem(item);
    };
    const auto add = [this](const core::Finding& f) {
        auto* item = new QListWidgetItem(QStringLiteral("● %1\n    %2").arg(qs(f.title), qs(f.detail)));
        item->setForeground(findingColor(f.level));
        item->setToolTip(QString::fromUtf8(core::toString(f.level).data()));
        m_findings->addItem(item);
    };

    if (m_comparison) {
        header(tr("Run comparison"));
        auto* item = new QListWidgetItem(qs(m_comparison->verdict));
        m_findings->addItem(item);
        m_findings->addItem(tr("    Run 1: %1 spikes (%2/min) · Run 2: %3 spikes (%4/min) · %5 at the same moments")
                                .arg(m_comparison->first.stats.spikes)
                                .arg(m_comparison->first.stats.spikesPerMinute, 0, 'f', 1)
                                .arg(m_comparison->second.stats.spikes)
                                .arg(m_comparison->second.stats.spikesPerMinute, 0, 'f', 1)
                                .arg(m_comparison->matchedSpikes));
    }

    const auto findings = currentFindings();
    if (!findings.empty()) {
        header(tr("This session"));
        for (const auto& f : findings)
            add(f);
    }

    if (!m_baselineFindings.empty()) {
        header(tr("Idle baseline"));
        for (const auto& f : m_baselineFindings)
            add(f);
    }

    if (m_findings->count() == 0)
        m_findings->addItem(tr("No findings yet. Capture with System trace on, or run the idle baseline."));
}

void MainWindow::appendSpikeRow(const core::Spike& s)
{
    const int row = m_spikeTable->rowCount();
    m_spikeTable->insertRow(row);
    m_spikeTable->setItem(row, ColTime, numberItem(s.timeSeconds, 2));
    m_spikeTable->setItem(row, ColFrame, numberItem(s.frameTimeMs, 1));
    m_spikeTable->setItem(row, ColExcess, numberItem(s.excessMs(), 1));
    m_spikeTable->setItem(row, ColBound, textItem(core::toString(s.bound)));

    if (!s.attribution) {
        for (int c : {ColSide, ColCause, ColConfidence, ColDetail})
            m_spikeTable->setItem(row, c, textItem(c == ColSide && m_trace.isRunning() ? "…" : "—"));
        return;
    }
    const auto& a = *s.attribution;
    auto* side = textItem(core::toString(a.side));
    side->setBackground(sideColor(a.side));
    m_spikeTable->setItem(row, ColSide, side);
    m_spikeTable->setItem(row, ColCause, textItem(core::toString(a.cause)));
    auto* conf = numberItem(a.confidence * 100.0, 0);
    conf->setData(Qt::DisplayRole, QStringLiteral("%1%").arg(qRound(a.confidence * 100.0)));
    m_spikeTable->setItem(row, ColConfidence, conf);
    m_spikeTable->setItem(row, ColDetail, textItem(a.detail));
}

void MainWindow::rebuildSpikeTable()
{
    m_spikeTable->setSortingEnabled(false);
    m_spikeTable->setRowCount(0);
    for (const auto& s : m_session.spikes())
        appendSpikeRow(s);
    m_spikeTable->setSortingEnabled(true);
}

void MainWindow::updateVerdict()
{
    if (m_mode == Mode::Baseline)
        return; // the countdown owns the label
    if (m_session.spikes().empty()) {
        m_verdictLabel->setText(m_mode == Mode::Capture   ? tr("Capturing … no stutter so far.")
                                : m_session.frames().empty() ? QString{}
                                                             : tr("No stutter detected."));
        return;
    }
    const auto d = m_session.diagnosis();
    if (d.attributed == 0) {
        m_verdictLabel->setText(m_trace.isRunning()
                                    ? tr("%1 spikes, waiting for system trace data …").arg(m_session.spikes().size())
                                    : tr("%1 spikes. Capture live with System trace on to find their causes.")
                                          .arg(m_session.spikes().size()));
        return;
    }
    m_verdictLabel->setText(tr("Verdict: %1").arg(qs(d.verdict)));
}

void MainWindow::logLatencySummary()
{
    const auto worst = m_latency.worst(5);
    if (worst.empty())
        return;
    log(tr("Worst drivers by single DPC/ISR:"));
    for (const auto& m : worst) {
        log(QStringLiteral("  %1: max %2 ms, %3× over 1 ms")
                .arg(qs(m.module))
                .arg(m.maxUs() / 1000.0, 0, 'f', 2)
                .arg(m.over1ms));
    }
}

void MainWindow::refresh()
{
    m_dirty = false;
    m_view->update();

    // Stats are O(n); twice a second is plenty.
    static qint64 lastStats = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_mode == Mode::Capture && now - lastStats < 500)
        return;
    lastStats = now;

    if (m_mode == Mode::Capture)
        updateVerdict();

    const auto s = m_session.stats();
    if (s.frames == 0) {
        m_statsLabel->setText(tr("No data"));
        return;
    }
    m_statsLabel->setText(tr("%1 frames · %2 s · avg %3 fps · 1% low %4 fps · worst %5 ms · "
                             "%6 spikes (%7/min)")
                              .arg(s.frames)
                              .arg(s.durationSeconds, 0, 'f', 1)
                              .arg(s.averageFps, 0, 'f', 0)
                              .arg(s.onePercentLowFps, 0, 'f', 0)
                              .arg(s.worstFrameMs, 0, 'f', 1)
                              .arg(s.spikes)
                              .arg(s.spikesPerMinute, 0, 'f', 1));
}

void MainWindow::log(const QString& message)
{
    m_log->appendPlainText(QStringLiteral("[%1] %2")
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")), message));
}

} // namespace framescope::ui
