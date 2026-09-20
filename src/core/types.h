#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>

namespace core {

struct ProxyNode {
    QString name;
    QString type;
    int delay = -1;  // milliseconds; -1 = untested, 0 = timeout
};

struct ProxyGroup {
    QString name;
    QString type;   // Selector, URLTest, Fallback, ...
    QString now;    // currently selected member
    QStringList all;

    bool selectable() const { return type == "Selector"; }
};

struct Connection {
    QString id;
    QString host;           // metadata.host, or destinationIP when host is empty
    QString network;        // tcp / udp
    QString connectionType; // metadata.type, e.g. HTTP, Socks5
    QStringList chains;     // proxy chain, outermost first
    QString rule;
    QString rulePayload;
    QString sourceIp;
    QString destinationPort;
    QString process;
    quint64 upload = 0;
    quint64 download = 0;
    QDateTime start;
};

struct LogEntry {
    QString level;  // info / warning / error / debug
    QString payload;
    QDateTime time;
};

struct Rule {
    QString type;
    QString payload;
    QString proxy;
};

/// Mutable slice of mihomo's /configs that the UI can read and patch.
struct BaseConfig {
    QString mode;      // rule / global / direct
    QString logLevel;
    quint16 mixedPort = 0;
    quint16 httpPort = 0;
    quint16 socksPort = 0;
    quint16 redirPort = 0;
    quint16 tproxyPort = 0;
    bool allowLan = false;
    bool ipv6 = false;
    bool tunEnabled = false;
};

struct Endpoint {
    QString host = "127.0.0.1";
    quint16 port = 9090;
    QString secret;

    QString httpBase() const { return QString("http://%1:%2").arg(host).arg(port); }
    QString wsBase() const { return QString("ws://%1:%2").arg(host).arg(port); }
    bool isValid() const { return port != 0 && !host.isEmpty(); }
};

}  // namespace core
