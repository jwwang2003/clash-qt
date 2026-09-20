#include "platform/browser_launcher.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QUrl>

#ifdef Q_OS_MACOS
#include <CoreServices/CoreServices.h>
#endif

namespace platform {
namespace {

void appendUnique(QVector<Browser> &browsers, const Browser &browser) {
    for (const Browser &known : browsers) {
        if (known.id.compare(browser.id, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    browsers.append(browser);
}

void markDefault(QVector<Browser> &browsers, const QString &defaultId) {
    if (defaultId.isEmpty()) {
        return;
    }
    for (int i = 0; i < browsers.size(); ++i) {
        if (browsers[i].id.compare(defaultId, Qt::CaseInsensitive) == 0) {
            browsers[i].isDefault = true;
            browsers.move(i, 0);
            return;
        }
    }
}

#ifdef Q_OS_MACOS

constexpr const char *kMacBundles[] = {
    "Safari.app", "Google Chrome.app", "Firefox.app", "Microsoft Edge.app",
    "Brave Browser.app", "Arc.app", "Vivaldi.app", "Opera.app",
    "Chromium.app", "Zen.app", "Orion.app", "LibreWolf.app",
};

bool readBundle(const QString &path, Browser &browser) {
    const QByteArray nativePath = path.toUtf8();
    CFURLRef bundleUrl = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(nativePath.constData()),
        nativePath.size(), true);
    if (!bundleUrl) {
        return false;
    }
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, bundleUrl);
    CFRelease(bundleUrl);
    if (!bundle) {
        return false;
    }
    if (CFStringRef identifier = CFBundleGetIdentifier(bundle)) {
        browser.id = QString::fromCFString(identifier);
    }
    const CFTypeRef name = CFBundleGetValueForInfoDictionaryKey(bundle, kCFBundleNameKey);
    if (name && CFGetTypeID(name) == CFStringGetTypeID()) {
        browser.name = QString::fromCFString(static_cast<CFStringRef>(name));
    }
    CFRelease(bundle);
    if (browser.name.isEmpty()) {
        browser.name = QFileInfo(path).completeBaseName();
    }
    return !browser.id.isEmpty();
}

QString defaultBundlePath() {
    // Launch Services resolves a non-file URL by its scheme alone, so this
    // probe URL is never fetched. The scheme-only entry points were removed in
    // 10.15, and the supported replacement is Objective-C only.
    CFURLRef probe = CFURLCreateWithString(kCFAllocatorDefault, CFSTR("http://localhost"), nullptr);
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    CFURLRef handler = LSCopyDefaultApplicationURLForURL(probe, kLSRolesAll, nullptr);
    QT_WARNING_POP
    CFRelease(probe);
    if (!handler) {
        return {};
    }
    CFStringRef path = CFURLCopyFileSystemPath(handler, kCFURLPOSIXPathStyle);
    CFRelease(handler);
    if (!path) {
        return {};
    }
    const QString result = QString::fromCFString(path);
    CFRelease(path);
    return result;
}

QVector<Browser> macBrowsers() {
    QVector<Browser> browsers;
    // The default handler can be a browser the probe list below does not know.
    Browser handler;
    if (const QString path = defaultBundlePath(); !path.isEmpty() && readBundle(path, handler)) {
        browsers.append(handler);
    }

    const QStringList roots{QStringLiteral("/Applications"),
                            QDir::homePath() + QStringLiteral("/Applications")};
    for (const QString &root : roots) {
        for (const char *bundleName : kMacBundles) {
            const QString path = root + QLatin1Char('/') + QLatin1String(bundleName);
            Browser browser;
            if (QFileInfo::exists(path) && readBundle(path, browser)) {
                appendUnique(browsers, browser);
            }
        }
    }
    markDefault(browsers, handler.id);
    return browsers;
}

bool macOpen(const QUrl &url, const QString &browserId) {
    // `open` exits successfully before it has resolved the target, so an
    // unknown browser has to be caught here for the caller's fallback to fire.
    const bool isPath = browserId.startsWith(QLatin1Char('/'));
    if (isPath) {
        if (!QFileInfo::exists(browserId)) {
            return false;
        }
    } else {
        CFStringRef identifier = browserId.toCFString();
        QT_WARNING_PUSH
        QT_WARNING_DISABLE_DEPRECATED
        CFArrayRef matches = LSCopyApplicationURLsForBundleIdentifier(identifier, nullptr);
        QT_WARNING_POP
        CFRelease(identifier);
        if (!matches) {
            return false;
        }
        CFRelease(matches);
    }

    const QString selector = isPath ? QStringLiteral("-a") : QStringLiteral("-b");
    return QProcess::startDetached(QStringLiteral("open"),
                                   {selector, browserId, url.toString()});
}

#endif  // Q_OS_MACOS

#ifdef Q_OS_WIN

const auto kClientsKey = QStringLiteral(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Clients\StartMenuInternet)");
const auto kUserChoiceKey =
    QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\Shell)"
                   R"(\Associations\UrlAssociations\http\UserChoice)");

QString registryValue(const QString &key, const QString &name) {
    return QSettings(key, QSettings::NativeFormat).value(name).toString();
}

QVector<Browser> windowsBrowsers() {
    QVector<Browser> browsers;
    QSettings clients(kClientsKey, QSettings::NativeFormat);
    for (const QString &client : clients.childGroups()) {
        const QString clientKey = kClientsKey + QLatin1Char('\\') + client;
        Browser browser;
        browser.id = registryValue(clientKey + QStringLiteral(R"(\Capabilities\URLAssociations)"),
                                   QStringLiteral("http"));
        if (browser.id.isEmpty()) {
            browser.id = client;
        }
        browser.name = registryValue(clientKey, QStringLiteral("Default"));
        if (browser.name.isEmpty()) {
            browser.name = client;
        }
        appendUnique(browsers, browser);
    }
    markDefault(browsers, registryValue(kUserChoiceKey, QStringLiteral("ProgId")));
    return browsers;
}

bool windowsOpen(const QUrl &url, const QString &browserId) {
    QString command =
        registryValue(QStringLiteral(R"(HKEY_CLASSES_ROOT\%1\shell\open\command)").arg(browserId),
                      QStringLiteral("Default"));
    if (command.isEmpty()) {
        command = registryValue(kClientsKey + QLatin1Char('\\') + browserId +
                                    QStringLiteral(R"(\shell\open\command)"),
                                QStringLiteral("Default"));
    }

    QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) {
        return false;
    }
    const QString program = parts.takeFirst();
    bool substituted = false;
    for (QString &argument : parts) {
        if (argument.contains(QLatin1String("%1"))) {
            argument.replace(QLatin1String("%1"), url.toString());
            substituted = true;
        }
    }
    if (!substituted) {
        parts.append(url.toString());
    }
    return QProcess::startDetached(program, parts);
}

#endif  // Q_OS_WIN

#ifdef Q_OS_LINUX

QStringList applicationDirs() {
    return {QStringLiteral("/usr/share/applications"),
            QDir::homePath() + QStringLiteral("/.local/share/applications")};
}

QString desktopEntryPath(const QString &desktopId) {
    for (const QString &dir : applicationDirs()) {
        const QString path = dir + QLatin1Char('/') + desktopId;
        if (QFileInfo::exists(path)) {
            return path;
        }
    }
    return {};
}

QVector<Browser> linuxBrowsers() {
    QProcess xdgSettings;
    xdgSettings.start(QStringLiteral("xdg-settings"),
                      {QStringLiteral("get"), QStringLiteral("default-web-browser")});
    QString defaultId;
    if (xdgSettings.waitForFinished(2000)) {
        defaultId = QString::fromUtf8(xdgSettings.readAllStandardOutput()).trimmed();
    }

    QVector<Browser> browsers;
    for (const QString &dir : applicationDirs()) {
        const QFileInfoList entries =
            QDir(dir).entryInfoList({QStringLiteral("*.desktop")}, QDir::Files, QDir::Name);
        for (const QFileInfo &entry : entries) {
            QSettings desktop(entry.absoluteFilePath(), QSettings::IniFormat);
            desktop.beginGroup(QStringLiteral("Desktop Entry"));
            if (desktop.value(QStringLiteral("NoDisplay")).toBool() ||
                desktop.value(QStringLiteral("Hidden")).toBool()) {
                continue;
            }
            const QStringList categories =
                desktop.value(QStringLiteral("Categories")).toString().split(QLatin1Char(';'));
            if (!categories.contains(QStringLiteral("WebBrowser"))) {
                continue;
            }
            Browser browser;
            browser.id = entry.fileName();
            browser.name =
                desktop.value(QStringLiteral("Name"), entry.completeBaseName()).toString();
            appendUnique(browsers, browser);
        }
    }
    markDefault(browsers, defaultId);
    return browsers;
}

bool linuxOpen(const QUrl &url, const QString &browserId) {
    const QString path = desktopEntryPath(browserId);
    if (path.isEmpty()) {
        return false;
    }
    QSettings desktop(path, QSettings::IniFormat);
    desktop.beginGroup(QStringLiteral("Desktop Entry"));
    QStringList parts = QProcess::splitCommand(desktop.value(QStringLiteral("Exec")).toString());
    if (parts.isEmpty()) {
        return false;
    }

    const QString program = parts.takeFirst();
    QStringList arguments;
    bool substituted = false;
    for (const QString &part : parts) {
        if (part == QLatin1String("%u") || part == QLatin1String("%U")) {
            arguments.append(url.toString());
            substituted = true;
        } else if (!part.startsWith(QLatin1Char('%'))) {
            // Field codes other than the URL ones expand to nothing we can supply.
            arguments.append(part);
        }
    }
    if (!substituted) {
        arguments.append(url.toString());
    }
    return QProcess::startDetached(program, arguments);
}

#endif  // Q_OS_LINUX

}  // namespace

QVector<Browser> BrowserLauncher::available() {
#if defined(Q_OS_MACOS)
    return macBrowsers();
#elif defined(Q_OS_WIN)
    return windowsBrowsers();
#elif defined(Q_OS_LINUX)
    return linuxBrowsers();
#else
    return {};
#endif
}

bool BrowserLauncher::open(const QUrl &url, const QString &browserId) {
    if (browserId.isEmpty()) {
        return QDesktopServices::openUrl(url);
    }
#if defined(Q_OS_MACOS)
    return macOpen(url, browserId);
#elif defined(Q_OS_WIN)
    return windowsOpen(url, browserId);
#elif defined(Q_OS_LINUX)
    return linuxOpen(url, browserId);
#else
    return QDesktopServices::openUrl(url);
#endif
}

}  // namespace platform
