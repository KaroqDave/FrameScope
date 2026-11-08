#include "capture/PresentMonCapture.h"

namespace framescope::capture {

// PresentMon 2.x flag names. Check `PresentMon --help` if your version differs.
namespace flags {
constexpr auto processName = "--process_name";
constexpr auto outputStdout = "--output_stdout";
constexpr auto qpcTime = "--qpc_time";                     // raw QPC times -> match ETW events
constexpr auto sessionName = "--session_name";
constexpr auto stopExisting = "--stop_existing_session";      // take over a stale session
constexpr auto terminateExisting = "--terminate_existing_session"; // stop a session, then exit
} // namespace flags

PresentMonCapture::PresentMonCapture(QObject* parent)
    : QObject(parent)
{
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &PresentMonCapture::onStdout);
    connect(&m_process, &QProcess::readyReadStandardError, this, &PresentMonCapture::onStderr);
    connect(&m_process, &QProcess::started, this, &PresentMonCapture::started);
    connect(&m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
        onStdout(); // flush whatever is left
        emit stopped(exitCode);
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart)
            emit failed(tr("Could not start PresentMon: %1").arg(m_process.errorString()));
    });
}

PresentMonCapture::~PresentMonCapture()
{
    stop();
}

bool PresentMonCapture::isRunning() const
{
    return m_process.state() != QProcess::NotRunning;
}

void PresentMonCapture::start(const Options& options)
{
    if (isRunning())
        return;
    if (options.presentMonPath.isEmpty() || options.processName.isEmpty()) {
        emit failed(tr("PresentMon path and process name are required."));
        return;
    }

    m_parser.reset();
    m_parser.setQpcFrequency(options.qpcFrequency);
    m_lineBuffer.clear();
    m_presentMonPath = options.presentMonPath;

    QStringList args{
        flags::processName, options.processName,
        flags::outputStdout,
        flags::qpcTime,
        flags::sessionName, kSessionName,
        flags::stopExisting,
    };
    args += options.extraArgs;

    m_commandLine = QStringLiteral("%1 %2").arg(options.presentMonPath, args.join(' '));
    emit logMessage(m_commandLine);
    m_process.start(options.presentMonPath, args);
}

void PresentMonCapture::stop()
{
    if (!isRunning())
        return;

    // A console app doesn't react to QProcess::terminate() on Windows, and a
    // hard kill leaves the ETW session running. So ask a second PresentMon
    // instance to end our named session; the capture process then exits.
    QProcess::execute(m_presentMonPath,
                      {flags::terminateExisting, flags::sessionName, kSessionName});

    if (!m_process.waitForFinished(3000)) {
        m_process.kill();
        m_process.waitForFinished(1000);
    }
}

void PresentMonCapture::onStdout()
{
    m_lineBuffer += m_process.readAllStandardOutput();

    std::vector<core::FrameRecord> frames;
    qsizetype start = 0;
    for (qsizetype nl = m_lineBuffer.indexOf('\n', start); nl >= 0;
         nl = m_lineBuffer.indexOf('\n', start)) {
        const auto line = std::string_view{m_lineBuffer.constData() + start,
                                           static_cast<std::size_t>(nl - start)};
        if (auto rec = m_parser.feedLine(line))
            frames.push_back(std::move(*rec));
        start = nl + 1;
    }
    m_lineBuffer.remove(0, start);

    if (!frames.empty())
        emit framesCaptured(frames);
}

void PresentMonCapture::onStderr()
{
    const auto text = QString::fromLocal8Bit(m_process.readAllStandardError()).trimmed();
    if (!text.isEmpty())
        emit logMessage(text);
}

} // namespace framescope::capture
