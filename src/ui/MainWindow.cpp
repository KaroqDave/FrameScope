#include "ui/MainWindow.h"

#include "core/PresentMonCsvParser.h"
#include "ui/FrameTimeView.h"

#include <QDateTime>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <exception>

namespace framescope::ui {
namespace {

constexpr auto kKeyPresentMon = "capture/presentMonPath";
constexpr auto kKeyProcess = "capture/processName";

enum SpikeColumn { ColTime, ColFrame, ColMedian, ColExcess, ColBound, ColCount };

QTableWidgetItem* numberItem(double value, int decimals)
{
    auto* item = new QTableWidgetItem;
    item->setData(Qt::DisplayRole, QString::number(value, 'f', decimals));
    item->setData(Qt::UserRole, value);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    buildUi();

    QSettings settings;
    m_presentMonPath->setText(settings.value(kKeyPresentMon).toString());
    m_processName->setText(settings.value(kKeyProcess).toString());

    connect(&m_capture, &capture::PresentMonCapture::framesCaptured, this, &MainWindow::onFrames);
    connect(&m_capture, &capture::PresentMonCapture::logMessage, this, &MainWindow::log);
    connect(&m_capture, &capture::PresentMonCapture::started, this, [this] {
        log(tr("Capture started. Waiting for frames from %1 …").arg(m_processName->text()));
    });
    connect(&m_capture, &capture::PresentMonCapture::stopped, this, [this](int code) {
        log(tr("PresentMon exited (code %1).").arg(code));
        setCapturing(false);
        refresh();
    });
    connect(&m_capture, &capture::PresentMonCapture::failed, this, [this](const QString& msg) {
        log(msg);
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

    setCapturing(false);
    refresh();
}

MainWindow::~MainWindow()
{
    QSettings settings;
    settings.setValue(kKeyPresentMon, m_presentMonPath->text());
    settings.setValue(kKeyProcess, m_processName->text());
    m_capture.stop();
}

void MainWindow::buildUi()
{
    setWindowTitle(tr("FrameScope"));
    resize(1200, 760);

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

    m_startButton = new QPushButton(tr("Start capture"));
    m_stopButton = new QPushButton(tr("Stop"));
    m_openButton = new QPushButton(tr("Open CSV…"));
    connect(m_startButton, &QPushButton::clicked, this, &MainWindow::startCapture);
    connect(m_stopButton, &QPushButton::clicked, this, &MainWindow::stopCapture);
    connect(m_openButton, &QPushButton::clicked, this, &MainWindow::openCsv);
    connect(m_processName, &QLineEdit::returnPressed, this, &MainWindow::startCapture);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_processName, 1);
    buttonRow->addWidget(m_startButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addSpacing(16);
    buttonRow->addWidget(m_openButton);

    auto* form = new QFormLayout;
    form->addRow(tr("PresentMon:"), pathRow);
    form->addRow(tr("Process:"), buttonRow);

    // ---- Graph
    m_view = new FrameTimeView;
    m_view->setSession(&m_session);

    // ---- Spike table
    m_spikeTable = new QTableWidget(0, ColCount);
    m_spikeTable->setHorizontalHeaderLabels(
        {tr("Time (s)"), tr("Frame (ms)"), tr("Median (ms)"), tr("Excess (ms)"), tr("Bound")});
    m_spikeTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_spikeTable->verticalHeader()->setVisible(false);
    m_spikeTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_spikeTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_spikeTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_spikeTable->setSortingEnabled(true);
    m_spikeTable->setToolTip(tr("Bound: which side was busy in the spike frame.\n"
                                "Stall = neither CPU nor GPU was busy — waiting on "
                                "I/O, paging, drivers or sync."));
    connect(m_spikeTable, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto rows = m_spikeTable->selectionModel()->selectedRows(ColTime);
        if (!rows.isEmpty())
            m_view->focusTime(rows.first().data(Qt::UserRole).toDouble());
    });

    // ---- Log
    m_log = new QPlainTextEdit;
    m_log->setReadOnly(true);
    m_log->setMaximumBlockCount(2000);

    auto* bottom = new QSplitter(Qt::Horizontal);
    bottom->addWidget(m_spikeTable);
    bottom->addWidget(m_log);
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
}

void MainWindow::startCapture()
{
    if (m_capture.isRunning())
        return;

    m_session.clear();
    m_spikeTable->setRowCount(0);
    m_view->setFollow(true);
    m_view->setSpanSeconds(30.0);
    setCapturing(true);

    m_capture.start({
        .presentMonPath = m_presentMonPath->text().trimmed(),
        .processName = m_processName->text().trimmed(),
        .extraArgs = {},
    });
}

void MainWindow::stopCapture()
{
    log(tr("Stopping capture …"));
    m_capture.stop();
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
        for (auto& f : frames)
            m_session.append(std::move(f));
    } catch (const std::exception& e) {
        QMessageBox::warning(this, tr("Could not open CSV"), QString::fromUtf8(e.what()));
        return;
    }

    rebuildSpikeTable();
    m_view->showAll();
    log(tr("Loaded %1 frames from %2").arg(m_session.frames().size()).arg(path));
    refresh();
}

void MainWindow::onFrames(const std::vector<core::FrameRecord>& frames)
{
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
    m_spikeTable->setItem(row, ColMedian, numberItem(s.medianMs, 1));
    m_spikeTable->setItem(row, ColExcess, numberItem(s.excessMs(), 1));
    m_spikeTable->setItem(row, ColBound,
                          new QTableWidgetItem(QString::fromUtf8(core::toString(s.bound).data())));
}

void MainWindow::rebuildSpikeTable()
{
    m_spikeTable->setSortingEnabled(false);
    m_spikeTable->setRowCount(0);
    for (const auto& s : m_session.spikes())
        appendSpikeRow(s);
    m_spikeTable->setSortingEnabled(true);
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
