#include "ui/Settings.h"

#include "win/SystemSampler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace framescope::ui {
namespace {

bool canWriteIn(const QString& dir)
{
    // QFileInfo::isWritable() doesn't reflect Windows ACLs reliably; just try it.
    QFile probe(QDir(dir).filePath(QStringLiteral(".framescope-write-test")));
    if (!probe.open(QIODevice::WriteOnly))
        return false;
    probe.close();
    probe.remove();
    return true;
}

} // namespace

QString settingsFile()
{
    static const QString path = [] {
        const auto appDir = QCoreApplication::applicationDirPath();
        const auto portable = QDir(appDir).filePath(QStringLiteral("framescope.ini"));
        if (QFileInfo::exists(portable) || canWriteIn(appDir))
            return portable;
        const auto configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        QDir().mkpath(configDir);
        return QDir(configDir).filePath(QStringLiteral("framescope.ini"));
    }();
    return path;
}

void migrateLegacySettings()
{
#ifdef _WIN32
    if (QFileInfo::exists(settingsFile()))
        return;
    QSettings legacy; // registry: HKCU\Software\FrameScope\FrameScope
    const auto keys = legacy.allKeys();
    if (!keys.isEmpty()) {
        QSettings ini(settingsFile(), QSettings::IniFormat);
        for (const auto& key : keys)
            ini.setValue(key, legacy.value(key));
        ini.sync();
        if (ini.status() != QSettings::NoError)
            return; // keep the registry copy if the ini couldn't be written
    }
    win::deleteLegacyRegistrySettings();
#endif
}

} // namespace framescope::ui
