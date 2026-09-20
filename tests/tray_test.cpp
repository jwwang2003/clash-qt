#include <QtTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include "core/mihomo_client.h"
#include "ui/proxy_environment.h"
#include "ui/tray_proxy_menu.h"

class TrayTest : public QObject {
    Q_OBJECT
private slots:
    void shellCommandsUseProxyPortsAndNoControllerSecret() {
        core::Endpoint endpoint{"127.0.0.1", 29097, "never-copy-this-secret"};
        core::BaseConfig config;
        config.mixedPort = 27890;
        config.httpPort = 1111;
        config.socksPort = 2222;
        const auto posix = ui::proxyEnvironment(endpoint, config, ui::Shell::Posix);
        QCOMPARE(posix, QString("export http_proxy='http://127.0.0.1:27890'\n"
                                "export https_proxy='http://127.0.0.1:27890'\n"
                                "export all_proxy='socks5h://127.0.0.1:27890'"));
        QVERIFY(!posix.contains(endpoint.secret));
        QVERIFY(!posix.contains("29097"));
        config.mixedPort = 0;
        endpoint.host = "::1";
        const auto ps = ui::proxyEnvironment(endpoint, config, ui::Shell::PowerShell);
        QCOMPARE(ps, QString("$env:http_proxy = 'http://[::1]:1111'\n"
                            "$env:https_proxy = 'http://[::1]:1111'\n"
                            "$env:all_proxy = 'socks5h://[::1]:2222'"));
    }

    void missingListenersClearObsoleteShellVariables() {
        core::Endpoint endpoint;
        core::BaseConfig config;
        QVERIFY(ui::proxyEnvironment(endpoint, config, ui::Shell::Posix).isEmpty());
        config.httpPort = 8080;
        QVERIFY(ui::proxyEnvironment(endpoint, config, ui::Shell::Posix).endsWith("unset all_proxy"));
        config.httpPort = 0;
        config.socksPort = 1080;
        QVERIFY(ui::proxyEnvironment(endpoint, config, ui::Shell::Posix)
                    .startsWith("unset http_proxy\nunset https_proxy\n"));
        QVERIFY(ui::proxyEnvironment(endpoint, config, ui::Shell::PowerShell)
                    .contains("Remove-Item Env:https_proxy -ErrorAction SilentlyContinue"));
    }

    void trayMenusFollowSnapshotsAndRejectStaleChoices() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QList<QByteArray> requests;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (auto *socket = server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [socket, &requests] {
                    QByteArray data = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", data);
                    const int headerEnd = data.indexOf("\r\n\r\n");
                    if (headerEnd < 0 || socket->property("answered").toBool()) return;
                    int contentLength = 0;
                    for (const auto &line : data.left(headerEnd).split('\n'))
                        if (line.toLower().startsWith("content-length:")) contentLength = line.mid(15).trimmed().toInt();
                    if (data.size() < headerEnd + 4 + contentLength) return;
                    socket->setProperty("answered", true);
                    requests.append(data);
                    const auto path = data.split(' ').value(1);
                    const QByteArray body = path == "/version" ? "{\"version\":\"test\"}"
                        : path == "/proxies" ? "{\"proxies\":{}}"
                        : path == "/rules" ? "{\"rules\":[]}" : "{}";
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            }
        });
        core::MihomoClient client;
        ui::TrayProxyMenu menu(&client);
        client.setEndpoint({"127.0.0.1", server.serverPort(), "test-auth"});
        QTRY_VERIFY(client.isConnected());
        QTRY_COMPARE(requests.size(), 4);
        QTest::qWait(20);
        const core::ProxyGroup group{"A&B / group", "Selector", "DIRECT", {"DIRECT", "Tokyo & Seoul"}};
        client.proxiesUpdated({group}, {});
        QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
        QCOMPARE(menu.actions().size(), 1);
        auto *submenu = menu.actions().first()->menu();
        QVERIFY(submenu);
        QCOMPARE(submenu->title(), QString("A&&B / group"));
        QCOMPARE(submenu->actions().at(1)->text(), QString("Tokyo && Seoul"));
        QVERIFY(submenu->actions().first()->isChecked());
        submenu->actions().at(1)->trigger();
        QTRY_VERIFY(std::any_of(requests.cbegin(), requests.cend(), [](const QByteArray &request) {
            return request.startsWith("PUT /proxies/A%26B%20%2F%20group ") &&
                   request.contains("Tokyo & Seoul") && request.contains("Authorization: Bearer test-auth");
        }));
        QTest::qWait(30);
        client.proxiesUpdated({group}, {});
        QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
        auto *staleAction = menu.actions().first()->menu()->actions().at(1);
        client.proxiesUpdated({}, {});
        const int before = requests.size();
        staleAction->trigger();
        QTest::qWait(30);
        QCOMPARE(requests.size(), before);
        QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
        QCOMPARE(menu.actions().size(), 1);
        QVERIFY(!menu.actions().first()->isEnabled());
        QVERIFY(menu.findChildren<QMenu *>().isEmpty());
    }

    void automaticAndReadOnlyGroupsHaveCorrectActions() {
        core::MihomoClient client;
        ui::TrayProxyMenu menu(&client);
        client.proxiesUpdated({
            {"Automatic", "URLTest", "DIRECT", {"DIRECT"}, {}},
            {"Balance", "LoadBalance", {}, {"DIRECT"}, {}}
        }, {});
        QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
        auto *automatic = menu.actions().first()->menu();
        QVERIFY(automatic->actions().first()->isChecked());
        auto *balanced = menu.actions().last()->menu();
        QVERIFY(!balanced->actions().first()->isEnabled());
        for (int i = 0; i < 3; ++i) {
            QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
            QCOMPARE(menu.findChildren<QMenu *>().size(), 2);
        }
    }
};

QTEST_MAIN(TrayTest)
#include "tray_test.moc"
