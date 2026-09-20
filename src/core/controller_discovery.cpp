#include "core/controller_discovery.h"

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace core {
namespace {

constexpr auto kVergeAppId = "io.github.clash-verge-rev.clash-verge-rev";

std::optional<Endpoint> fromEnvironment() {
    const auto env = QProcessEnvironment::systemEnvironment();
    const QString controller = env.value("CLASH_QT_CONTROLLER");
    if (controller.isEmpty()) return std::nullopt;

    const qsizetype colon = controller.lastIndexOf(':');
    if (colon < 0) return std::nullopt;

    Endpoint endpoint;
    endpoint.host = controller.left(colon);
    endpoint.port = controller.mid(colon + 1).toUShort();
    endpoint.secret = env.value("CLASH_QT_SECRET");
    if (endpoint.host.isEmpty() || endpoint.host == "0.0.0.0") endpoint.host = "127.0.0.1";
    return endpoint.isValid() ? std::optional(endpoint) : std::nullopt;
}

}  // namespace

QString vergeConfigPath() {
#ifdef Q_OS_MACOS
    const QString root = QDir::homePath() + "/Library/Application Support/" + kVergeAppId;
#elif defined(Q_OS_WIN)
    const QString root =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/" + kVergeAppId;
#else
    const QString root = QDir::homePath() + "/.local/share/" + kVergeAppId;
#endif
    return root + "/config.yaml";
}

std::optional<Endpoint> endpointFromConfigFile(const QString &path) {
    if (!QFileInfo::exists(path)) return std::nullopt;

    try {
        const YAML::Node root = YAML::LoadFile(path.toStdString());
        const YAML::Node controller = root["external-controller"];
        if (!controller || !controller.IsScalar()) return std::nullopt;

        const QString value = QString::fromStdString(controller.as<std::string>());
        const qsizetype colon = value.lastIndexOf(':');
        if (colon < 0) return std::nullopt;

        Endpoint endpoint;
        endpoint.host = value.left(colon);
        endpoint.port = value.mid(colon + 1).toUShort();
        // mihomo binds 0.0.0.0 to listen everywhere; we still dial it locally.
        if (endpoint.host.isEmpty() || endpoint.host == "0.0.0.0") endpoint.host = "127.0.0.1";

        if (const YAML::Node secret = root["secret"]; secret && secret.IsScalar()) {
            endpoint.secret = QString::fromStdString(secret.as<std::string>());
        }
        return endpoint.isValid() ? std::optional(endpoint) : std::nullopt;
    } catch (const YAML::Exception &) {
        return std::nullopt;
    }
}

Endpoint discoverEndpoint() {
    if (auto fromEnv = fromEnvironment()) return *fromEnv;
    if (auto fromVerge = endpointFromConfigFile(vergeConfigPath())) return *fromVerge;
    return Endpoint{};
}

}  // namespace core
