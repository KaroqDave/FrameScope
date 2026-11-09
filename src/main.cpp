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

    framescope::ui::MainWindow window;
    window.show();
    return QApplication::exec();
}
