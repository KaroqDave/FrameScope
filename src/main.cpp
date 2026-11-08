#include "ui/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("FrameScope"));
    QApplication::setApplicationName(QStringLiteral("FrameScope"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    framescope::ui::MainWindow window;
    window.show();
    return QApplication::exec();
}
