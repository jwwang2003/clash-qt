#pragma once

#include <optional>

#include "core/types.h"

namespace core {

/// Locates a mihomo external controller without requiring the user to configure one.
/// Order: CLASH_QT_CONTROLLER/CLASH_QT_SECRET env vars, then Clash Verge Rev's own
/// config.yaml, then a bare localhost default.
Endpoint discoverEndpoint();

/// Parses `external-controller` and `secret` out of a mihomo config.yaml.
std::optional<Endpoint> endpointFromConfigFile(const QString &path);

/// Platform location of Clash Verge Rev's generated runtime config.
QString vergeConfigPath();

}  // namespace core
