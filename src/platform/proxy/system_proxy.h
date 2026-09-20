#pragma once

#include <QString>
#include <QMetaType>

namespace platform {

struct ProxyConfig {
    QString host = "127.0.0.1";
    quint16 port = 0;
    QString bypass;  // comma-separated hosts that skip the proxy
    quint16 socksPort = 0;  // 0 disables SOCKS; HTTP/HTTPS use port
};

struct SystemProxyState {
    ProxyConfig config;
    bool supported = false;
    bool valid = false;
    bool owned = false;
    QString error;
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

    /// Restores the settings captured before enable, only while our settings
    /// still match. Another application's subsequent changes are left alone.
    /// macOS may retain a dormant endpoint when the original proxy was empty/off.
    static bool restoreOwned();
    static bool ownsProxy();

    /// What the OS currently reports. `port == 0` means no proxy is set.
    static ProxyConfig current();
    /// One serialized read of the effective configuration and ownership.
    static SystemProxyState state();
    static bool isEnabled();

    /// Human-readable reason the last call failed; empty when it succeeded.
    static QString lastError();
};

}  // namespace platform

Q_DECLARE_METATYPE(platform::ProxyConfig)
Q_DECLARE_METATYPE(platform::SystemProxyState)
