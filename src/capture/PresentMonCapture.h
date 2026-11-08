#pragma once

#include "core/FrameRecord.h"
#include "core/PresentMonCsvParser.h"

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace framescope::capture {

// Runs PresentMon as a child process and streams its CSV output.
// No injection or overlay hooks: PresentMon reads ETW events, which keeps
// this compatible with anti-cheat.
//
// Needs admin rights (or membership in "Performance Log Users").
class PresentMonCapture : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString presentMonPath;   // full path to PresentMon-x.y.z-x64.exe
        QString processName;      // e.g. "cod.exe"
        QStringList extraArgs;    // passed through unchanged
        std::int64_t qpcFrequency = 10'000'000; // QueryPerformanceFrequency
    };

    static constexpr auto kSessionName = "FrameScope";

    explicit PresentMonCapture(QObject* parent = nullptr);
    ~PresentMonCapture() override;

    void start(const Options& options);
    void stop();
    [[nodiscard]] bool isRunning() const;

signals:
    // Batched per stdout read to keep UI overhead low at high frame rates.
    void framesCaptured(const std::vector<framescope::core::FrameRecord>& frames);
    void started();
    void stopped(int exitCode);
    void failed(const QString& message);
    void logMessage(const QString& message);

private:
    void onStdout();
    void onStderr();

    QProcess m_process;
    QString m_presentMonPath;
    core::PresentMonCsvParser m_parser;
    QByteArray m_lineBuffer;
};

} // namespace framescope::capture
