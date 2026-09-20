#include "ui/proxy_environment.h"

#include <QStringList>
#include <QUrl>

namespace ui {
namespace {
QString proxyUrl(const core::Endpoint &endpoint, quint16 port, const QString &scheme) {
    if (!port) return {};
    QUrl url(endpoint.httpBase());
    if (!url.isValid() || url.host().isEmpty()) return {};
    url.setScheme(scheme);
    url.setPort(port);
    return url.toString(QUrl::FullyEncoded);
}

QString quote(QString text, Shell shell) {
    text.replace('\'', shell == Shell::Posix ? "'\\''" : "''");
    return '\'' + text + '\'';
}
}  // namespace

QString proxyEnvironment(const core::Endpoint &endpoint, const core::BaseConfig &config, Shell shell) {
    const QString http = proxyUrl(endpoint, config.mixedPort ? config.mixedPort : config.httpPort, "http");
    const QString socks = proxyUrl(endpoint, config.mixedPort ? config.mixedPort : config.socksPort, "socks5h");
    if (http.isEmpty() && socks.isEmpty()) return {};
    QStringList commands;
    const std::pair<QString, QString> variables[] = {
        {"http_proxy", http}, {"https_proxy", http}, {"all_proxy", socks}
    };
    for (const auto &[name, value] : variables) {
        if (shell == Shell::Posix) {
            commands << (value.isEmpty() ? "unset " + name : "export " + name + '=' + quote(value, shell));
        } else {
            commands << (value.isEmpty() ? "Remove-Item Env:" + name + " -ErrorAction SilentlyContinue"
                                        : "$env:" + name + " = " + quote(value, shell));
        }
    }
    return commands.join('\n');
}

}  // namespace ui
