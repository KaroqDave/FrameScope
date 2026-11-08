#include "ui/MainWindow.h"

#include "core/PresentMonCsvParser.h"
#include "ui/FrameTimeView.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>

namespace framescope::ui {
namespace {

constexpr auto kKeyPresentMon = "capture/presentMonPath";
constexpr auto kKeyProcess = "capture/processName";
constexpr auto kKeyTrace = "capture/systemTrace";

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

// Spikes are attributed once the trace has covered them. CPU buckets lag
// ~2 s behind (per-CPU ETW buffers), so wait a bit longer than that.
constexpr double kSettleMs = 4000.0;

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

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , m_timeline(etw::KernelTrace::qpcFrequency())
{
    m_correlatorConfig.processName = [](std::uint32_t pid) { return win::processName(pid); };
    buildUi();

    QSettings settings;
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
        m_trace.stop();
        m_sampler.stop();
        m_traceTimer->stop();
        setCapturing(false);
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

    // Pull ETW events and attribute settled spikes once a second.
    m_traceTimer = new QTimer(this);
    m_traceTimer->setInterval(1000);
    connect(m_traceTimer, &QTimer::timeout, this, [this] { pumpTrace(false); });

    setCapturing(false);
    refresh();
}

MainWindow::~MainWindow()
{
    QSettings settings;
    settings.setValue(kKeyPresentMon, m_presentMonPath->text());
    settings.setValue(kKeyProcess, m_processName->text());
    settings.setValue(kKeyTrace, m_traceCheck->isChecked());
    m_capture.stop();
    m_trace.stop();
    m_sampler.stop();
}

void MainWindow::buildUi()
{
    setWindowTitle(tr("FrameScope"));
    resize(1280, 800);

    // ---- Capture controls
    m_presentMonPath = new QLineEdit;
    m_presentMonPath->setPlaceholderText(tr("Path to PresentMon-2.x-x64.exe"));
    auto* browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::browsePresentMon);

    auto* pathRow = new QHBoxLayout;
    pathRow->addWidget(m_presentMonPath, 1);
    pathRow->addWidget(browse);

    m_processName = new QLineEdit;
    m_processName->setPlaceholderText(tr("Game executable, e.g. cod.exe"));

    m_traceCheck = new QCheckBox(tr("System trace"));
    m_traceCheck->setToolTip(etw::KernelTrace::isSupported()
                                 ? tr("Record driver latency, page faults, disk I/O, CPU scheduling, "
                                      "RAM/VRAM and GPU sensors to find out what caused each spike.")
                                 : tr("Only available on Windows."));
    m_traceCheck->setEnabled(etw::KernelTrace::isSupported());

    m_startButton = new QPushButton(tr("Start capture"));
    m_stopButton = new QPushButton(tr("Stop"));
    m_openButton = new QPushButton(tr("Open CSV…"));
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startCapture);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopCapture);
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::openCsv);
    connect(m_processName, &QLineEdit::returnPressed, this, &MainWindow::startCapture);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_processName, 1);
    buttonRow->addWidget(m_traceCheck);
    buttonRow->addWidget(m_startButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addSpacing(16);
    buttonRow->addWidget(m_openButton);

    auto* form = new QFormLayout;
    form->addRow(tr("PresentMon:"), pathRow);
    form->addRow(tr("Process:"), buttonRow);

    // ---- Verdict
    m_verdictLabel = new QLabel;
    m_verdictLabel->setWordWrap(true);
    QFont vf = m_verdictLabel->font();
    vf.setPointSizeF(vf.pointSizeF() * 1.15);
    vf.setBold(true);
    m_verdictLabel->setFont(vf);

    // ---- Graph
    m_view = new FrameTimeView;
    m_view->setSession(&m_session);

    // ---- Spike table
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
                                "Side/Cause appear ~2 s after a spike, once the system trace has covered it."));
    connect(m_spikeTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = m_spikeTable->selectionModel()->selectedRows(ColTime);
        if (!rows.isEmpty())
            m_view->focusTime(rows.first().data(Qt::UserRole).toDouble());
    });

    // ---- Findings + log
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

void MainWindow::setCapturing(bool capturing)
{
    m_startButton->setEnabled(!capturing);
    m_stopButton->setEnabled(capturing);
    m_openButton->setEnabled(!capturing);
    m_presentMonPath->setEnabled(!capturing);
    m_processName->setEnabled(!capturing);
    m_traceCheck->setEnabled(!capturing && etw::KernelTrace::isSupported());
}

void MainWindow::startCapture()
{
    if (m_capture.isRunning())
        return;

    m_session.clear();
    m_timeline.clear();
    m_spikeTable->setRowCount(0);
    m_view->setFollow(true);
    m_view->setSpanSeconds(30.0);
    m_warnedNoDpc = false;
    m_loggedSensors = false;
    m_gamePid = 0;
    m_findings->clear();
    setCapturing(true);

    // Start the system trace first so it covers the very first frames.
    if (m_traceCheck->isChecked()) {
        const auto topo = win::queryCpuTopology();
        m_timeline.setTopology(topo);
        if (topo.hasAsymmetricCache())
            log(tr("CPU has 3D V-Cache on CCD%1: checking where the game's threads run.").arg(topo.vcacheCcd));

        etw::KernelTraceOptions opts;
        opts.ccdOfCpu = topo.ccdOfCpu;
        m_sampler.start(250);

        if (m_trace.start(opts)) {
            m_timeline.setTraceStart(m_trace.startQpc());
            log(tr("System trace running (%1).").arg(QString::fromStdString(m_trace.sessionDescription())));
            m_traceTimer->start();
        } else {
            log(tr("System trace unavailable: %1. Continuing with frame capture only.")
                    .arg(QString::fromStdString(m_trace.lastError())));
            m_sampler.stop();
        }
    }

    m_capture.start({
        .presentMonPath = m_presentMonPath->text().trimmed(),
        .processName = m_processName->text().trimmed(),
        .extraArgs = {},
        .qpcFrequency = etw::KernelTrace::qpcFrequency(),
    });
    updateVerdict();
}

void MainWindow::stopCapture()
{
    log(tr("Stopping capture …"));
    m_capture.stop(); // emits stopped() -> onCaptureStopped()
}

void MainWindow::onCaptureStopped(int exitCode)
{
    log(tr("PresentMon exited (code %1).").arg(exitCode));
    if (m_trace.isRunning() || m_traceTimer->isActive()) {
        m_traceTimer->stop();
        m_trace.stop();   // flushes the last CPU buckets
        m_sampler.stop();
        pumpTrace(true);
        logLatencySummary();
        updateFindings();
    }
    setCapturing(false);
    refresh();
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
        log(tr("Sensors: %1").arg(QString::fromStdString(m_sampler.status())));
    }

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

    // A session that starts fine but delivers nothing is the most likely
    // failure mode on unusual systems; say so instead of failing silently.
    const auto runningFor = m_timeline.toMicroseconds(etw::KernelTrace::qpcNow() - m_trace.startQpc());
    if (!m_warnedNoDpc && !final && runningFor > 3e6 && m_trace.dpcIsrCount() == 0) {
        m_warnedNoDpc = true;
        log(tr("Warning: the system trace delivers no DPC/ISR events. "
               "Run framescope-latency --nt-kernel-logger to check."));
    }
}

void MainWindow::browsePresentMon()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Select PresentMon"),
                                                   m_presentMonPath->text(),
                                                   tr("Executables (*.exe);;All files (*)"));
    if (!path.isEmpty())
        m_presentMonPath->setText(path);
}

void MainWindow::openCsv()
{
    const auto path = QFileDialog::getOpenFileName(this, tr("Open PresentMon CSV"), {},
                                                   tr("CSV files (*.csv);;All files (*)"));
    if (path.isEmpty())
        return;

    try {
        auto frames = core::loadPresentMonCsv(path.toStdWString());
        m_session.clear();
        m_timeline.clear();
        m_findings->clear();
        for (auto& f : frames)
            m_session.append(std::move(f));
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
    refresh();
}

void MainWindow::onFrames(const std::vector<core::FrameRecord>& frames)
{
    if (m_gamePid == 0 && !frames.empty() && frames.front().processId != 0) {
        m_gamePid = frames.front().processId;
        m_trace.setTargetProcess(m_gamePid);
        m_sampler.setTargetProcess(m_gamePid);
        log(tr("Game process: PID %1").arg(m_gamePid));
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
    if (m_session.spikes().empty()) {
        m_verdictLabel->setText(m_capture.isRunning() ? tr("Capturing … no stutter so far.") : QString{});
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
    m_verdictLabel->setText(tr("Verdict: %1").arg(QString::fromStdString(d.verdict)));
}

void MainWindow::logLatencySummary()
{
    const auto worst = m_trace.latencySnapshot().worst(5);
    if (worst.empty())
        return;
    log(tr("Worst drivers by single DPC/ISR:"));
    for (const auto& m : worst) {
        log(QStringLiteral("  %1: max %2 ms, %3× over 1 ms")
                .arg(QString::fromStdString(m.module))
                .arg(m.maxUs() / 1000.0, 0, 'f', 2)
                .arg(m.over1ms));
    }
}

void MainWindow::updateFindings()
{
    const auto latency = m_trace.latencySnapshot();
    const auto findings = core::sessionFindings(m_timeline, latency.empty() ? nullptr : &latency, m_correlatorConfig);
    m_findings->clear();
    for (const auto& f : findings) {
        auto* item = new QListWidgetItem(QStringLiteral("● %1\n    %2")
                                             .arg(QString::fromStdString(f.title), QString::fromStdString(f.detail)));
        item->setForeground(findingColor(f.level));
        item->setToolTip(QString::fromUtf8(core::toString(f.level).data()));
        m_findings->addItem(item);
    }
    if (findings.empty())
        m_findings->addItem(tr("No findings yet. Capture with System trace on."));
}

void MainWindow::refresh()
{
    m_dirty = false;
    m_view->update();

    // Stats are O(n); twice a second is plenty.
    static qint64 lastStats = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_capture.isRunning() && now - lastStats < 500)
        return;
    lastStats = now;

    if (m_capture.isRunning())
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
                               .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                    message));
}

} // namespace framescope::ui
