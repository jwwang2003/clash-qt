#include "core/mihomo/controller_discovery.h"

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QtGlobal>

namespace core {
namespace {

constexpr auto kVergeAppId = "io.github.clash-verge-rev.clash-verge-rev";

/// mihomo binds 0.0.0.0 / :: to listen everywhere; we still dial it locally.
/// Brackets are stripped here so one host string is stored for `[::1]` and
/// `::1` - Endpoint::baseUrl() puts them back for the URL.
QString normaliseHost(QString host) {
    host = host.trimmed();
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2).trimmed();
    if (host.isEmpty() || host == QLatin1String("0.0.0.0")) return QStringLiteral("127.0.0.1");
    if (host == QLatin1String("::")) return QStringLiteral("::1");
    return host;
}

}  // namespace

Endpoint noControllerEndpoint() {
    Endpoint endpoint;
    endpoint.host.clear();
    endpoint.port = 0;
    endpoint.secret.clear();
    return endpoint;
}

std::optional<Endpoint> endpointFromAuthority(const QString &value) {
    const QString authority = value.trimmed();
    if (authority.isEmpty()) return std::nullopt;

    QString host;
    QString portText;
    if (authority.startsWith(QLatin1Char('['))) {
        // A bracketed literal owns every colon inside it, so lastIndexOf(':')
        // would split "[::1]" - no port at all - into host "[::" and port "1]".
        const qsizetype close = authority.indexOf(QLatin1Char(']'));
        if (close < 0) return std::nullopt;
        const QString rest = authority.mid(close + 1);
        if (!rest.startsWith(QLatin1Char(':'))) return std::nullopt;
        host = authority.left(close + 1);
        portText = rest.mid(1);
    } else {
        const qsizetype colon = authority.lastIndexOf(QLatin1Char(':'));
        if (colon < 0) return std::nullopt;
        host = authority.left(colon);
        portText = authority.mid(colon + 1);
    }

    bool parsed = false;
    const uint port = portText.trimmed().toUInt(&parsed);
    // Checked, not merely converted: toUShort() answers 0 for "nine thousand"
    // and for "70000" alike, and an unchecked 0 used to read as "no port found"
    // in one place and as a dialable endpoint in another.
    if (!parsed || port == 0 || port > 65535) return std::nullopt;

    Endpoint endpoint;
    endpoint.host = normaliseHost(host);
    endpoint.port = static_cast<quint16>(port);
    return endpoint.isValid() ? std::optional(endpoint) : std::nullopt;
}

ControllerDiscoveryInputs processDiscoveryInputs() {
    ControllerDiscoveryInputs inputs;
    inputs.explicitControllerSet = qEnvironmentVariableIsSet("CLASH_QT_CONTROLLER");
    inputs.explicitController = qEnvironmentVariable("CLASH_QT_CONTROLLER");
    inputs.explicitSecret = qEnvironmentVariable("CLASH_QT_SECRET");
    inputs.isolated = qEnvironmentVariableIsSet("CLASH_QT_DATA_DIR");
    // Left EMPTY for an isolated process. The gate in discoverEndpoint() is
    // what enforces the rule, but a path that is never even assembled cannot
    // be read by a later caller that forgets to ask about isolation first.
    if (!inputs.isolated) inputs.foreignConfigPath = vergeConfigPath();
    return inputs;
}

QString vergeConfigPath() {
#ifdef Q_OS_MACOS
    const QString root = QDir::homePath() + "/Library/Application Support/" + kVergeAppId;
#elif defined(Q_OS_WIN)
    const QString root = qEnvironmentVariable("APPDATA", QDir::homePath() + "/AppData/Roaming") + "/" + kVergeAppId;
#else
    const QString root = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/" + kVergeAppId;
#endif
    return root + "/config.yaml";
}

std::optional<Endpoint> endpointFromConfigFile(const QString &path) {
    if (path.isEmpty() || !QFileInfo::exists(path)) return std::nullopt;

    try {
        const YAML::Node root = YAML::LoadFile(path.toStdString());
        const YAML::Node controller = root["external-controller"];
        if (!controller || !controller.IsScalar()) return std::nullopt;

        std::optional<Endpoint> endpoint =
            endpointFromAuthority(QString::fromStdString(controller.as<std::string>()));
        if (!endpoint) return std::nullopt;

        if (const YAML::Node secret = root["secret"]; secret && secret.IsScalar()) {
            endpoint->secret = QString::fromStdString(secret.as<std::string>());
        }
        return endpoint;
    } catch (const YAML::Exception &) {
        return std::nullopt;
    }
}

Endpoint discoverEndpoint(const ControllerDiscoveryInputs &inputs) {
    // 1. The host said where. That answer is the whole answer: a value that
    //    does not parse means no controller, NOT "go and look somewhere else".
    if (inputs.explicitControllerSet) {
        std::optional<Endpoint> explicitEndpoint =
            endpointFromAuthority(inputs.explicitController);
        if (!explicitEndpoint) return noControllerEndpoint();
        explicitEndpoint->secret = inputs.explicitSecret;
        return *explicitEndpoint;
    }

    // 2. A selected data directory is a process that owns its own world. It
    //    attaches to nothing it was not given: no other installation's
    //    configuration is read, and the conventional 127.0.0.1:9090 is not
    //    assumed, because that address usually belongs to somebody else's
    //    running core and driving it is not this process's business.
    if (inputs.isolated) return noControllerEndpoint();

    // 3. The ordinary launch, unchanged: the neighbouring installation's
    //    generated config, then the bare localhost default.
    if (auto foreign = endpointFromConfigFile(inputs.foreignConfigPath)) return *foreign;
    return Endpoint{};
}

Endpoint discoverEndpoint() { return discoverEndpoint(processDiscoveryInputs()); }

}  // namespace core
