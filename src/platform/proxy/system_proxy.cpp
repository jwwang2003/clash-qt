#include "platform/proxy/system_proxy.h"

#include <QFileInfo>
#include <QMap>
#include <QMutex>
#include <QVariant>
#include <QLibrary>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStringList>

namespace platform {
namespace {

thread_local QString g_lastError;
QRecursiveMutex g_proxyMutex;

#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX)

/// Runs a configuration tool and captures its output. networksetup reports its
/// failures on stdout rather than stderr, so both feed `g_lastError`.
bool run(const QString &program, const QStringList &arguments, QString *output = nullptr) {
#ifdef CLASH_QT_PROXY_TEST_RUNNER
    const bool result = CLASH_QT_PROXY_TEST_RUNNER(program, arguments, output);
    if (!result) g_lastError = QStringLiteral("Mock proxy command failed.");
    return result;
#else
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(5000)) {
        g_lastError = QStringLiteral("%1: %2").arg(program, process.errorString());
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    const QString out = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        out.startsWith("Error:", Qt::CaseInsensitive) || out.contains("** Error")) {
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
#endif
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
    QString route;
    QString interface;
    for (const QStringList &arguments : {QStringList{"-n", "get", "default"},
                                        QStringList{"-n", "get", "-inet6", "default"}}) {
        if (!run("/sbin/route", arguments, &route)) continue;
        for (const QString &line : route.split('\n')) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith("interface:")) interface = trimmed.mid(10).trimmed();
        }
        if (!interface.isEmpty()) break;
    }
    QString order;
    if (!interface.isEmpty() && run(kNetworksetup, {"-listnetworkserviceorder"}, &order)) {
        QString candidate;
        static const QRegularExpression serviceLine(R"(^\(\d+\)\s+(.+)$)");
        for (const QString &line : order.split('\n')) {
            const auto match = serviceLine.match(line.trimmed());
            if (match.hasMatch()) candidate = match.captured(1);
            else if (!candidate.isEmpty() && line.contains("Device: " + interface + ')')) {
                g_lastError.clear();
                return candidate;
            }
            else if (line.trimmed().startsWith('(')) candidate.clear();
        }
    }
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
            g_lastError.clear();
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
    for (int i = 0; i < 3; ++i) {
        if (i == 2 && config.socksPort == 0) {
            if (!run(kNetworksetup, {QLatin1String(kStateSetters[i]), service, "off"})) return false;
            continue;
        }
        const QString port = QString::number(i == 2 ? config.socksPort : config.port);
        // Assigning a server also switches that protocol's proxy on.
        if (!run(kNetworksetup, {QLatin1String(kProxySetters[i]), service, config.host, port})) {
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
        // networksetup uses the literal sentinel "Empty" to clear the list.
        arguments.append(QStringLiteral("Empty"));
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
    using SetOption = int (__stdcall *)(void *, unsigned long, void *, unsigned long);
    const auto setOption = reinterpret_cast<SetOption>(QLibrary::resolve("wininet", "InternetSetOptionW"));
    if (!setOption || !setOption(nullptr, 39, nullptr, 0) || !setOption(nullptr, 37, nullptr, 0)) {
        g_lastError = QStringLiteral("Could not notify Windows of the proxy change.");
        return false;
    }
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

struct ProxySnapshot {
    QString service;
    QMap<QString, QVariant> values;
    bool valid = false;
};
ProxySnapshot originalSettings;
ProxySnapshot ownedSettings;

#ifdef Q_OS_MACOS
const char *const kGetters[] = {"-getwebproxy", "-getsecurewebproxy", "-getsocksfirewallproxy",
                               "-getproxybypassdomains", "-getautoproxyurl", "-getproxyautodiscovery"};
QString reportField(const QString &report, const QString &name) {
    for (const QString &line : report.split('\n')) {
        if (line.startsWith(name + ':')) return line.mid(name.size() + 1).trimmed();
    }
    return {};
}
#endif

ProxySnapshot snapshot(const QString &service = {}) {
    ProxySnapshot state;
#ifdef Q_OS_MACOS
    state.service = service.isEmpty() ? primaryService() : service;
    if (state.service.isEmpty()) return state;
    for (const char *getter : kGetters) {
        QString output;
        if (!run(kNetworksetup, {QLatin1String(getter), state.service}, &output)) return state;
        state.values.insert(QLatin1String(getter), output);
    }
#elif defined(Q_OS_WIN)
    Q_UNUSED(service);
    QSettings settings(kInternetSettings, QSettings::NativeFormat);
    for (const char *key : {"ProxyEnable", "ProxyServer", "ProxyOverride", "AutoConfigURL"}) {
        state.values.insert(QLatin1String(key), settings.value(QLatin1String(key)));
    }
    if (settings.status() != QSettings::NoError) return state;
#elif defined(Q_OS_LINUX)
    Q_UNUSED(service);
    for (const char *child : {"http", "https", "socks"}) {
        const QString schema = kProxySchema + '.' + QLatin1String(child);
        for (const char *key : {"host", "port"}) {
            QString output;
            if (!run(gsettings(), {"get", schema, QLatin1String(key)}, &output)) return state;
            state.values.insert(schema + '/' + QLatin1String(key), output);
        }
    }
    for (const char *key : {"mode", "ignore-hosts"}) {
        QString output;
        if (!run(gsettings(), {"get", kProxySchema, QLatin1String(key)}, &output)) return state;
        state.values.insert(kProxySchema + '/' + QLatin1String(key), output);
    }
#else
    Q_UNUSED(service);
    return state;
#endif
    state.valid = true;
    return state;
}

bool restore(const ProxySnapshot &state) {
    if (!state.valid) return false;
    bool ok = true;
#ifdef Q_OS_MACOS
    for (int i = 0; i < 3; ++i) {
        const QString report = state.values.value(QLatin1String(kGetters[i])).toString();
        ProxyConfig config;
        const bool enabled = parseProxyReport(report, &config);
        // networksetup cannot restore an empty endpoint. For a previously
        // disabled proxy, restore its off state and leave the dormant endpoint.
        const bool validEndpoint = !config.host.trimmed().isEmpty() && config.port != 0;
        if (validEndpoint) {
            ok = run(kNetworksetup, {QLatin1String(kProxySetters[i]), state.service,
                                    config.host, QString::number(config.port)}) && ok;
        } else if (enabled) {
            g_lastError = QStringLiteral("Cannot restore an enabled proxy with an invalid endpoint.");
            ok = false;
        }
        ok = run(kNetworksetup, {QLatin1String(kStateSetters[i]), state.service,
                                enabled && validEndpoint ? "on" : "off"}) && ok;
    }
    const QString bypass = state.values.value("-getproxybypassdomains").toString();
    QStringList domains;
    for (const QString &line : bypass.split('\n', Qt::SkipEmptyParts)) {
        if (!line.contains(' ')) domains.append(line);
    }
    if (domains.isEmpty()) domains.append("Empty");
    ok = run(kNetworksetup, QStringList{"-setproxybypassdomains", state.service} + domains) && ok;
    const QString pac = state.values.value("-getautoproxyurl").toString();
    // The PAC URL itself is never modified; only its enabled state changes.
    ok = run(kNetworksetup, {"-setautoproxystate", state.service,
                            reportField(pac, "Enabled") == "Yes" ? "on" : "off"}) && ok;
    const QString discovery = state.values.value("-getproxyautodiscovery").toString();
    ok = run(kNetworksetup, {"-setproxyautodiscovery", state.service,
                            discovery.endsWith("On", Qt::CaseInsensitive) ? "on" : "off"}) && ok;
#elif defined(Q_OS_WIN)
    QSettings settings(kInternetSettings, QSettings::NativeFormat);
    for (auto it = state.values.begin(); it != state.values.end(); ++it) {
        if (it.value().isValid()) settings.setValue(it.key(), it.value());
        else settings.remove(it.key());
    }
    ok = commit(settings);
#elif defined(Q_OS_LINUX)
    for (auto it = state.values.begin(); it != state.values.end(); ++it) {
        if (it.key().endsWith("/mode")) continue;
        const int slash = it.key().lastIndexOf('/');
        ok = setKey(it.key().left(slash), it.key().mid(slash + 1), it.value().toString()) && ok;
    }
    ok = setKey(kProxySchema, "mode", state.values.value(kProxySchema + "/mode").toString()) && ok;
#endif
    return ok;
}

bool apply(const ProxyConfig &config, const QString &service) {
#if defined(Q_OS_MACOS)
    return applyProxies(service, config) &&
           run(kNetworksetup, {"-setautoproxystate", service, "off"}) &&
           run(kNetworksetup, {"-setproxyautodiscovery", service, "off"});
#elif defined(Q_OS_WIN)
    Q_UNUSED(service);
    QSettings settings(kInternetSettings, QSettings::NativeFormat);
    QString bypass = config.bypass;
    QString server = QString("http=%1:%2;https=%1:%2").arg(config.host).arg(config.port);
    if (config.socksPort != 0) server += QString(";socks=%1:%2").arg(config.host).arg(config.socksPort);
    settings.setValue("ProxyServer", server);
    settings.setValue("ProxyOverride", bypass.replace(',', ';'));
    settings.setValue("ProxyEnable", 1u);
    settings.remove("AutoConfigURL");
    return commit(settings);
#elif defined(Q_OS_LINUX)
    Q_UNUSED(service);
    auto quoted = [](QString value) {
        value.replace("\\", "\\\\").replace("'", "\\'");
        return "'" + value + "'";
    };
    for (const char *child : {"http", "https", "socks"}) {
        const QString schema = kProxySchema + '.' + QLatin1String(child);
        const bool socks = QLatin1String(child) == "socks";
        const quint16 port = socks ? config.socksPort : config.port;
        if (!setKey(schema, "host", quoted(port == 0 ? QString() : config.host)) ||
            !setKey(schema, "port", QString::number(port))) return false;
    }
    QStringList hosts;
    for (const QString &entry : config.bypass.split(',', Qt::SkipEmptyParts)) {
        hosts.append(quoted(entry.trimmed()));
    }
    return setKey(kProxySchema, "ignore-hosts", '[' + hosts.join(',') + ']') &&
           setKey(kProxySchema, "mode", "'manual'");
#else
    Q_UNUSED(config);
    Q_UNUSED(service);
    return false;
#endif
}

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
    const QMutexLocker lock(&g_proxyMutex);
    g_lastError.clear();
    if (config.host.trimmed().isEmpty() || config.host.contains('\n') || config.port == 0) {
        g_lastError = QStringLiteral("A nonempty proxy host and port between 1 and 65535 are required.");
        return false;
    }
    if (!isSupported()) {
        g_lastError = QStringLiteral("This platform has no supported proxy mechanism.");
        return false;
    }
    const ProxySnapshot before = snapshot();
    if (!before.valid) return false;
    if (ownedSettings.valid && ownedSettings.service != before.service && !restoreOwned()) return false;
#ifdef Q_OS_MACOS
    for (int i = 0; i < 3; ++i) {
        const QString report = before.values.value(QLatin1String(kGetters[i])).toString();
        if (reportField(report, "Authenticated Proxy Enabled") == "1") {
            g_lastError = QStringLiteral("Cannot replace an authenticated proxy because its credentials cannot be restored.");
            return false;
        }
    }
#endif
    const bool stillOwned = ownedSettings.valid && before.service == ownedSettings.service &&
                            before.values == ownedSettings.values;
    if (!apply(config, before.service)) {
        const QString reason = g_lastError;
        const bool restored = restore(before);
        if (!restored) {
            if (!stillOwned) originalSettings = before;
            ownedSettings = snapshot(before.service);
        }
        g_lastError = reason + (restored ? QString() : QStringLiteral(" Previous proxy settings could not be fully restored."));
        return false;
    }
    const ProxySnapshot after = snapshot(before.service);
    if (!after.valid) {
        const QString reason = g_lastError;
        const bool restored = restore(before);
        if (!restored) {
            if (!stillOwned) originalSettings = before;
            ownedSettings = snapshot(before.service);
        }
        g_lastError = reason + (restored ? QString() : QStringLiteral(" Previous proxy settings could not be fully restored."));
        return false;
    }
    if (!stillOwned) originalSettings = before;
    ownedSettings = after;
    g_lastError.clear();
    return true;
}

bool SystemProxy::ownsProxy() {
    const QMutexLocker lock(&g_proxyMutex);
    if (!ownedSettings.valid) return false;
    const ProxySnapshot now = snapshot(ownedSettings.service);
    return now.valid && now.service == ownedSettings.service && now.values == ownedSettings.values;
}

bool SystemProxy::restoreOwned() {
    const QMutexLocker lock(&g_proxyMutex);
    g_lastError.clear();
    if (!ownedSettings.valid) {
        if (!originalSettings.valid) return true;
        g_lastError = QStringLiteral("Cannot safely restore proxy settings because their current state could not be read.");
        return false;
    }
    const ProxySnapshot now = snapshot(ownedSettings.service);
    if (!now.valid) return false;
    if (now.values != ownedSettings.values || now.service != ownedSettings.service) {
        ownedSettings = {};
        originalSettings = {};
        return true;
    }
    if (!restore(originalSettings)) {
        const QString reason = g_lastError;
        ownedSettings = snapshot(ownedSettings.service);
        g_lastError = reason;
        return false;
    }
    ownedSettings = {};
    originalSettings = {};
    return true;
}

bool SystemProxy::disable() {
    const QMutexLocker lock(&g_proxyMutex);
    if (ownedSettings.valid) return restoreOwned();
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
    const QMutexLocker lock(&g_proxyMutex);
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
    ProxyConfig socks;
    if (run(kNetworksetup, {"-getsocksfirewallproxy", service}, &report) && parseProxyReport(report, &socks))
        config.socksPort = socks.port;
#elif defined(Q_OS_WIN)
    QSettings settings(kInternetSettings, QSettings::NativeFormat);
    if (settings.value(QStringLiteral("ProxyEnable"), 0u).toUInt() == 0) {
        return {};
    }
    const QString server = settings.value(QStringLiteral("ProxyServer")).toString();
    for (const QString &entry : server.split(';', Qt::SkipEmptyParts)) {
        const qsizetype equals = entry.indexOf('=');
        const QString protocol = equals < 0 ? QString() : entry.left(equals).trimmed();
        const QString address = equals < 0 ? entry : entry.mid(equals + 1);
        const qsizetype separator = address.lastIndexOf(':');
        if (separator <= 0) continue;
        if (protocol.isEmpty() || protocol == "http") {
            config.host = address.left(separator);
            config.port = address.mid(separator + 1).toUShort();
        }
        if (protocol.isEmpty() || protocol == "socks")
            config.socksPort = address.mid(separator + 1).toUShort();
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
    config.socksPort = getKey(kProxySchema + ".socks", "port").toUShort();
    config.bypass = getKey(kProxySchema, QStringLiteral("ignore-hosts"))
                        .remove(QLatin1Char('['))
                        .remove(QLatin1Char(']'))
                        .remove(QLatin1Char('\''))
                        .remove(QLatin1Char(' '));
#endif
    return config;
}

SystemProxyState SystemProxy::state() {
    const QMutexLocker lock(&g_proxyMutex);
    SystemProxyState result;
    result.supported = isSupported();
    if (!result.supported) {
        result.valid = true;
        return result;
    }
    result.config = current();
    result.error = g_lastError;
    if (!result.error.isEmpty()) return result;
    result.owned = ownsProxy();
    result.error = g_lastError;
    result.valid = result.error.isEmpty();
    return result;
}

bool SystemProxy::isEnabled() { return current().port != 0; }

QString SystemProxy::lastError() { return g_lastError; }

}  // namespace platform
