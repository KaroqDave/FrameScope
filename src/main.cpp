#include "etw/KernelTrace.h"
#include "ui/MainWindow.h"
#include "ui/Settings.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("FrameScope"));
    QApplication::setApplicationName(QStringLiteral("FrameScope"));
    QApplication::setApplicationVersion(QStringLiteral(FRAMESCOPE_VERSION));

    framescope::ui::migrateLegacySettings();

    // A crashed earlier run may have left its trace sessions running.
    framescope::etw::KernelTrace::stopStaleSessions(L"FrameScope");
    framescope::etw::KernelTrace::installCrashCleanup(L"FrameScope");

    framescope::ui::MainWindow window;
    window.show();
    // "framescope.exe capture.fscope" or a file dropped onto the exe opens it.
    if (const auto args = QApplication::arguments(); args.size() > 1)
        window.openPath(args.at(1));
    return QApplication::exec();
}
