#include "platform/system/autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QMutex>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#ifdef Q_OS_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace platform {
namespace {

thread_local QString g_lastError;
QRecursiveMutex g_autostartMutex;

QString executablePath() { return QCoreApplication::applicationFilePath(); }

#ifdef Q_OS_MACOS

QString agentLabel() {
    if (CFStringRef identifier = CFBundleGetIdentifier(CFBundleGetMainBundle())) {
        return QString::fromCFString(identifier);
    }
    // An unbundled build has no identifier; the label only has to be unique per user.
    return QStringLiteral("org.clash-qt.") + QCoreApplication::applicationName();
}

QString agentPath() {
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/") + agentLabel() +
           QStringLiteral(".plist");
}

QString agentPlist() {
    return QStringLiteral(R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>%1</string>
	<key>ProgramArguments</key>
	<array>
		<string>%2</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
</dict>
</plist>
)")
        .arg(agentLabel().toHtmlEscaped(), executablePath().toHtmlEscaped());
}

bool readsAsEnabled() {
    QFile file(agentPath());
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        g_lastError = file.errorString();
        return false;
    }
    // A stale entry left by a moved binary would never launch this build, so it
    // reads as disabled rather than as a silently broken "on".
    return QString::fromUtf8(file.readAll())
        .contains(QStringLiteral("<string>%1</string>").arg(executablePath().toHtmlEscaped()));
}

bool write(bool enabled) {
    const QString path = agentPath();
    if (!enabled) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            g_lastError = QStringLiteral("Cannot remove %1").arg(path);
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        g_lastError = QStringLiteral("Cannot create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        g_lastError = file.errorString();
        return false;
    }
    const QByteArray contents = agentPlist().toUtf8();
    if (file.write(contents) != contents.size() || !file.commit()) {
        g_lastError = file.errorString();
        return false;
    }
    return true;
}

#endif  // Q_OS_MACOS

#ifdef Q_OS_WIN

QSettings runKey() {
    return QSettings(
        QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)"),
        QSettings::NativeFormat);
}

bool readsAsEnabled() {
    QSettings settings = runKey();
    QString value = settings.value(QCoreApplication::applicationName()).toString();
    if (settings.status() != QSettings::NoError) {
        g_lastError = QStringLiteral("Cannot read the Run registry key");
        return false;
    }
    return value.remove(QLatin1Char('"')).compare(QDir::toNativeSeparators(executablePath()),
                                                  Qt::CaseInsensitive) == 0;
}

bool write(bool enabled) {
    QSettings run = runKey();
    if (enabled) {
        run.setValue(QCoreApplication::applicationName(),
                     QLatin1Char('"') + QDir::toNativeSeparators(executablePath()) +
                         QLatin1Char('"'));
    } else {
        run.remove(QCoreApplication::applicationName());
    }
    run.sync();
    if (run.status() != QSettings::NoError) {
        g_lastError = QStringLiteral("Cannot write the Run registry key");
        return false;
    }
    return true;
}

#endif  // Q_OS_WIN

#ifdef Q_OS_LINUX

QString desktopPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
           QStringLiteral("/autostart/") + QCoreApplication::applicationName() +
           QStringLiteral(".desktop");
}

QString desktopEntry() {
    QString quoted = executablePath();
    quoted.replace('\\', "\\\\\\\\").replace('"', "\\\\\"").replace('`', "\\\\`").replace('$', "\\\\$").replace('%', "%%");
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=%1\n"
                          "Exec=\"%2\"\n"
                          "Terminal=false\n"
                          "X-GNOME-Autostart-enabled=true\n")
        .arg(QCoreApplication::applicationName(), quoted);
}

bool readsAsEnabled() {
    QFile file(desktopPath());
    if (!file.exists()) return false;
    if (!file.open(QIODevice::ReadOnly)) { g_lastError = file.errorString(); return false; }
    return QString::fromUtf8(file.readAll()) == desktopEntry();
}

bool write(bool enabled) {
    const QString path = desktopPath();
    if (!enabled) {
        if (QFile::exists(path) && !QFile::remove(path)) {
            g_lastError = QStringLiteral("Cannot remove %1").arg(path);
            return false;
        }
        return true;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        g_lastError = QStringLiteral("Cannot create %1").arg(QFileInfo(path).absolutePath());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        g_lastError = file.errorString();
        return false;
    }
    const QByteArray contents = desktopEntry().toUtf8();
    if (file.write(contents) != contents.size() || !file.commit()) {
        g_lastError = file.errorString();
        return false;
    }
    return true;
}

#endif  // Q_OS_LINUX

}  // namespace

bool Autostart::isSupported() {
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

bool Autostart::isEnabled() {
    const QMutexLocker lock(&g_autostartMutex);
    g_lastError.clear();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return readsAsEnabled();
#else
    return false;
#endif
}

bool Autostart::setEnabled(bool enabled) {
    const QMutexLocker lock(&g_autostartMutex);
    g_lastError.clear();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN) || defined(Q_OS_LINUX)
    return write(enabled);
#else
    Q_UNUSED(enabled)
    g_lastError = QStringLiteral("Launch at login is not available on this platform");
    return false;
#endif
}

QString Autostart::lastError() { return g_lastError; }

}  // namespace platform
