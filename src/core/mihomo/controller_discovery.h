#pragma once

#include <optional>

#include "core/types.h"

namespace core {

/// Everything discovery is ALLOWED to consider, named rather than read.
///
/// WHY THIS IS A STRUCT. The old discoverEndpoint() read the environment and
/// another application's configuration file itself, so there was no way to ask
/// "what would this process attach to" without also asking the filesystem about
/// a client this project does not own. Every rule below is now a property of
/// the inputs, and the only function that touches the environment is
/// processDiscoveryInputs().
struct ControllerDiscoveryInputs {
    /// CLASH_QT_CONTROLLER, verbatim, and whether it was set at all.
    ///
    /// AUTHORITATIVE when `explicitControllerSet` is true. A malformed or empty
    /// value resolves to noControllerEndpoint() and MUST NOT fall back to
    /// `foreignConfigPath` or to the conventional port: a host that named a
    /// controller and got the name wrong asked for that controller, not for
    /// whichever other client happens to be installed.
    QString explicitController;
    QString explicitSecret;  // CLASH_QT_SECRET
    bool explicitControllerSet = false;

    /// True when this process was pointed at a selected data directory
    /// (CLASH_QT_DATA_DIR, which src/main.cpp also sets from --data-dir).
    ///
    /// An isolated process with no explicit controller attaches to NOTHING. It
    /// does not read `foreignConfigPath` and it does not assume 127.0.0.1:9090,
    /// because on a developer's or a user's machine that address is very often
    /// another client's live controller - and a smoke launch, a workflow suite
    /// or a second profile would then drive an engine it does not own.
    bool isolated = false;

    /// The other installation's generated config.yaml. Read ONLY by an ordinary
    /// non-isolated launch that was given no explicit controller, which is the
    /// legacy convenience this project shipped. processDiscoveryInputs() leaves
    /// it EMPTY for an isolated process, so the path is not merely unused there
    /// but absent.
    QString foreignConfigPath;
};

/// What the current process environment permits. The only environment read in
/// this header's implementation.
ControllerDiscoveryInputs processDiscoveryInputs();

/// "Attached to nothing", explicitly: empty host, port 0, no secret.
///
/// NOT the same as a default-constructed core::Endpoint, which is the localhost
/// DISCOVERY default 127.0.0.1:9090 and whose isValid() is true. Returning that
/// from discovery is what made an isolated launch attach to a controller nobody
/// named; a caller that wants "nothing" must say so with this.
Endpoint noControllerEndpoint();

/// Locates a mihomo external controller under the rules above:
///
///   1. an explicit CLASH_QT_CONTROLLER/CLASH_QT_SECRET wins outright, and a bad
///      one resolves to noControllerEndpoint() rather than to anything else;
///   2. an isolated process with no explicit controller gets
///      noControllerEndpoint();
///   3. an ordinary launch falls back, as it always has, to Clash Verge Rev's
///      own config.yaml and then to the bare localhost default.
///
/// The caller must check the result: cb::isValid()/Endpoint::isValid() is false
/// for cases 1-bad and 2, and attaching to an invalid endpoint is a dial to
/// whatever the default authority resolves to.
Endpoint discoverEndpoint();
Endpoint discoverEndpoint(const ControllerDiscoveryInputs &inputs);

/// Parses a `host:port` authority the way both a config file and the
/// environment variable spell it, including `[::1]:9090`, `[::]:9090` and
/// `0.0.0.0:9090`. nullopt when it is not one.
std::optional<Endpoint> endpointFromAuthority(const QString &value);

/// Parses `external-controller` and `secret` out of a mihomo config.yaml.
std::optional<Endpoint> endpointFromConfigFile(const QString &path);

/// Platform location of Clash Verge Rev's generated runtime config.
QString vergeConfigPath();

}  // namespace core
