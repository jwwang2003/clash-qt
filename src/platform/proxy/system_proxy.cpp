#include "platform/proxy/system_proxy.h"

#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

namespace platform {
namespace {

QString g_lastError;

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)

/// Runs a configuration tool and captures its output. networksetup reports its
/// failures on stdout rather than stderr, so both feed `g_lastError`.
bool run(const QString &program, const QStringList &arguments, QString *output = nullptr) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(5000)) {
        g_lastError = QStringLiteral("%1: %2").arg(program, process.errorString());
        return false;
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        g_lastError = err.isEmpty() ? out : err;
        if (g_lastError.isEmpty()) {
            g_lastError =
                QStringLiteral("%1 failed with code %2").arg(program).arg(process.exitCode());
        }
        return false;
    }
    if (output) {
        *output = out;
    }
    return true;
}

#endif

#ifdef Q_OS_MACOS

const auto kNetworksetup = QStringLiteral("/usr/sbin/networksetup");

const char *const kProxySetters[] = {"-setwebproxy", "-setsecurewebproxy",
                                     "-setsocksfirewallproxy"};
const char *const kStateSetters[] = {"-setwebproxystate", "-setsecurewebproxystate",
                                     "-setsocksfirewallproxystate"};

/// True when the service holds the default route, which is what makes macOS
/// treat it as primary. The IPv6 report prefixes the key, so only the bare one
/// counts.
bool hasDefaultRoute(const QString &info) {
    const auto key = QStringLiteral("Router: ");
    for (const QString &line : info.split(QLatin1Char('\n'))) {
        if (line.startsWith(key)) {
            return line.mid(key.size()).trimmed() != QLatin1String("none");
        }
    }
    return false;
}

/// The service the proxy applies to. `-listallnetworkservices` prints a legend
/// first, then the services in routing-priority order, marking the disabled
/// ones with a leading '*'.
QString primaryService() {
    QString listing;
    if (!run(kNetworksetup, {QStringLiteral("-listallnetworkservices")}, &listing)) {
        return {};
    }
    QStringList lines = listing.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    if (!lines.isEmpty()) {
        lines.removeFirst();
    }
    for (const QString &line : lines) {
        const QString service = line.trimmed();
        if (service.startsWith(QLatin1Char('*'))) {
            continue;
        }
        QString info;
        if (run(kNetworksetup, {QStringLiteral("-getinfo"), service}, &info) &&
            hasDefaultRoute(info)) {
            return service;
        }
    }
    g_lastError = QStringLiteral("No active network service to configure.");
    return {};
}

/// Fills `config` from a -getwebproxy style report and returns its Enabled flag.
bool parseProxyReport(const QString &report, ProxyConfig *config) {
    bool enabled = false;
    for (const QString &line : report.split(QLatin1Char('\n'))) {
        const qsizetype separator = line.indexOf(QLatin1Char(':'));
        if (separator < 0) {
            continue;
        }
        const QString key = line.left(separator).trimmed();
        const QString value = line.mid(separator + 1).trimmed();
        if (key == QLatin1String("Enabled")) {
            enabled = value.compare(QLatin1String("Yes"), Qt::CaseInsensitive) == 0;
        } else if (key == QLatin1String("Server")) {
            config->host = value;
        } else if (key == QLatin1String("Port")) {
            config->port = value.toUShort();
        }
    }
    return enabled;
}

QString readBypass(const QString &service) {
    QString report;
    if (!run(kNetworksetup, {QStringLiteral("-getproxybypassdomains"), service}, &report)) {
        return {};
    }
    QStringList entries;
    for (const QString &line : report.split(QLatin1Char('\n'))) {
        // An empty list is answered with a prose sentence; no real entry has a space.
        const QString entry = line.trimmed();
        if (!entry.isEmpty() && !entry.contains(QLatin1Char(' '))) {
            entries.append(entry);
        }
    }
    return entries.join(QLatin1Char(','));
}

bool applyProxies(const QString &service, const ProxyConfig &config) {
    const QString port = QString::number(config.port);
    for (const char *setter : kProxySetters) {
        // Assigning a server also switches that protocol's proxy on.
        if (!run(kNetworksetup, {QLatin1String(setter), service, config.host, port})) {
            return false;
        }
    }
    QStringList arguments{QStringLiteral("-setproxybypassdomains"), service};
    for (const QString &entry : config.bypass.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        if (const QString trimmed = entry.trimmed(); !trimmed.isEmpty()) {
            arguments.append(trimmed);
        }
    }
    if (arguments.size() == 2) {
        // A single empty argument is how networksetup is told to clear the list.
        arguments.append(QString());
    }
    return run(kNetworksetup, arguments);
}

bool setProxyStates(const QString &service, const QString &state) {
    for (const char *setter : kStateSetters) {
        if (!run(kNetworksetup, {QLatin1String(setter), service, state})) {
            return false;
        }
    }
    return true;
}

#endif  // Q_OS_MACOS

#ifdef Q_OS_WIN

const auto kInternetSettings = QStringLiteral(
    R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Internet Settings)");

bool commit(QSettings &settings) {
    settings.sync();
    if (settings.status() != QSettings::NoError) {
        g_lastError = QStringLiteral("Could not write %1.").arg(kInternetSettings);
        return false;
    }
    // Already-running applications only notice the change after a WinINet
    // InternetSetOption(INTERNET_OPTION_SETTINGS_CHANGED) broadcast, which we
    // cannot send without linking wininet.
    return true;
}

#endif  // Q_OS_WIN

#ifdef Q_OS_LINUX

const auto kProxySchema = QStringLiteral("org.gnome.system.proxy");

QString gsettings() { return QStandardPaths::findExecutable(QStringLiteral("gsettings")); }

bool setKey(const QString &schema, const QString &key, const QString &value) {
    return run(gsettings(), {QStringLiteral("set"), schema, key, value});
}

/// gsettings quotes the strings it prints; callers want the bare value.
QString getKey(const QString &schema, const QString &key) {
    QString value;
    if (!run(gsettings(), {QStringLiteral("get"), schema, key}, &value)) {
        return {};
    }
    if (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))) {
        value = value.mid(1, value.size() - 2);
    }
    return value;
}

#endif  // Q_OS_LINUX

}  // namespace

bool SystemProxy::isSupported() {
#if defined(Q_OS_MACOS)
    return QFileInfo::exists(kNetworksetup);
#elif defined(Q_OS_WIN)
    return true;
#elif defined(Q_OS_LINUX)
    return !gsettings().isEmpty();
#else
    return false;
#endif
}

bool SystemProxy::enable(const ProxyConfig &config) {
    g_lastError.clear();
#if defined(Q_OS_MACOS)
    const QString service = primaryService();
    return !service.isEmpty() && applyProxies(service, config);
#elif defined(Q_OS_WIN)
    QSettings settings(kInternetSettings, QSettings::NativeFormat);
    QString bypass = config.bypass;
    settings.setValue(QStringLiteral("ProxyServer"),
                      QStringLiteral("%1:%2").arg(config.host).arg(config.port));
    settings.setValue(QStringLiteral("ProxyOverride"),
                      bypass.replace(QLatin1Char(','), QLatin1Char(';')));
    settings.setValue(QStringLiteral("ProxyEnable"), 1u);
    return commit(settings);
#elif defined(Q_OS_LINUX)
    const QString port = QString::number(config.port);
    for (const char *child : {"http", "https", "socks"}) {
        const QString schema = kProxySchema + QLatin1Char('.') + QLatin1String(child);
        if (!setKey(schema, QStringLiteral("host"), config.host) ||
            !setKey(schema, QStringLiteral("port"), port)) {
            return false;
        }
    }
    QStringList hosts;
    for (const QString &entry : config.bypass.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        hosts.append(QLatin1Char('\'') + entry.trimmed() + QLatin1Char('\''));
    }
    // Switch the mode last, so the manual settings are in place when it takes effect.
    return setKey(kProxySchema, QStringLiteral("ignore-hosts"),
                  QLatin1Char('[') + hosts.join(QLatin1Char(',')) + QLatin1Char(']')) &&
           setKey(kProxySchema, QStringLiteral("mode"), QStringLiteral("manual"));
#else
    Q_UNUSED(config);
    g_lastError = QStringLiteral("This platform has no supported proxy mechanism.");
    return false;
#endif
}

bool SystemProxy::disable() {
    g_lastError.clear();
#if defined(Q_OS_MACOS)
    const QString service = primaryService();
    return !service.isEmpty() && setProxyStates(service, QStringLiteral("off"));
#elif defined(Q_OS_WIN)
    QSettings settings(kInternetSettings, QSettings::NativeFormat);
    settings.setValue(QStringLiteral("ProxyEnable"), 0u);
    return commit(settings);
#elif defined(Q_OS_LINUX)
    return setKey(kProxySchema, QStringLiteral("mode"), QStringLiteral("none"));
#else
    g_lastError = QStringLiteral("This platform has no supported proxy mechanism.");
    return false;
#endif
}

ProxyConfig SystemProxy::current() {
    g_lastError.clear();
    ProxyConfig config;
#if defined(Q_OS_MACOS)
    const QString service = primaryService();
    QString report;
    if (service.isEmpty() ||
        !run(kNetworksetup, {QStringLiteral("-getwebproxy"), service}, &report)) {
        return {};
    }
    // A switched-off proxy keeps its last server, which is not what the OS is
    // using; leaving the port at 0 is how the contract spells "no proxy".
    if (!parseProxyReport(report, &config)) {
        return {};
    }
    config.bypass = readBypass(service);
#elif defined(Q_OS_WIN)
    QSettings settings(kInternetSettings, QSettings::NativeFormat);
    if (settings.value(QStringLiteral("ProxyEnable"), 0u).toUInt() == 0) {
        return {};
    }
    const QString server = settings.value(QStringLiteral("ProxyServer")).toString();
    if (const qsizetype separator = server.lastIndexOf(QLatin1Char(':')); separator > 0) {
        config.host = server.left(separator);
        config.port = server.mid(separator + 1).toUShort();
    }
    config.bypass = settings.value(QStringLiteral("ProxyOverride")).toString().replace(
        QLatin1Char(';'), QLatin1Char(','));
#elif defined(Q_OS_LINUX)
    if (getKey(kProxySchema, QStringLiteral("mode")) != QLatin1String("manual")) {
        return {};
    }
    const QString schema = kProxySchema + QStringLiteral(".http");
    config.host = getKey(schema, QStringLiteral("host"));
    config.port = getKey(schema, QStringLiteral("port")).toUShort();
    config.bypass = getKey(kProxySchema, QStringLiteral("ignore-hosts"))
                        .remove(QLatin1Char('['))
                        .remove(QLatin1Char(']'))
                        .remove(QLatin1Char('\''))
                        .remove(QLatin1Char(' '));
#endif
    return config;
}

bool SystemProxy::isEnabled() { return current().port != 0; }

QString SystemProxy::lastError() { return g_lastError; }

}  // namespace platform
