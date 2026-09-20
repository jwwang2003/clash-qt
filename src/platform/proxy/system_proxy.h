#pragma once

#include <QString>

namespace platform {

struct ProxyConfig {
    QString host = "127.0.0.1";
    quint16 port = 0;
    QString bypass;  // comma-separated hosts that skip the proxy
};

/// Reads and writes the OS-level HTTP/HTTPS/SOCKS proxy settings.
///
/// Contract with the ui module. Extend, do not reshape.
class SystemProxy {
public:
    /// False when this platform has no supported mechanism, in which case the
    /// UI should say so rather than offer a control that silently does nothing.
    static bool isSupported();

    static bool enable(const ProxyConfig &config);
    static bool disable();

    /// What the OS currently reports. `port == 0` means no proxy is set.
    static ProxyConfig current();
    static bool isEnabled();

    /// Human-readable reason the last call failed; empty when it succeeded.
    static QString lastError();
};

}  // namespace platform
