#include <QtTest>
#include <QCheckBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "core/mihomo/mihomo_client.h"
#include "ui/shell/routing_controls.h"
#include "ui/theme/theme.h"

class RoutingServer : public QTcpServer {
public:
    bool enabled = false;
    bool rejectDevice = false;
    int patches = 0;
    RoutingServer() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QByteArray data = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", data);
                    const int end = data.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("answered").toBool()) return;
                    int length = 0;
                    for (const auto &line : data.left(end).split('\n'))
                        if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                    if (data.size() < end + 4 + length) return;
                    socket->setProperty("answered", true);
                    const auto words = data.left(data.indexOf('\r')).split(' ');
                    QByteArray body = "{}";
                    QByteArray status = "200 OK";
                    if (words[1] == "/version") body = R"({"version":"test"})";
                    else if (words[1] == "/proxies") body = R"({"proxies":{}})";
                    else if (words[1] == "/rules") body = R"({"rules":[]})";
                    else if (words[0] == "PATCH") {
                        ++patches;
                        const auto patch = QJsonDocument::fromJson(data.mid(end + 4)).object();
                        if (!rejectDevice) enabled = patch.value("tun").toObject().value("enable").toBool();
                        status = "204 No Content";
                        body.clear();
                    } else if (words[1] == "/configs") {
                        body = QJsonDocument(QJsonObject{{"mode", "rule"}, {"mixed-port", 7890},
                            {"tun", QJsonObject{{"enable", enabled}, {"auto-route", true},
                                               {"auto-detect-interface", true}, {"stack", "mixed"}}}}).toJson();
                    }
                    QTimer::singleShot(15, socket, [socket, status, body] {
                        socket->write("HTTP/1.1 " + status + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                                      "\r\nConnection: close\r\n\r\n" + body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
    }
    core::Endpoint endpoint() const { return {"127.0.0.1", serverPort(), "test"}; }
};

class RoutingControlsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { ui::theme::install(); }

    void systemProxySwitchMirrorsActualStateWithoutFeedback() {
        core::MihomoClient client;
        ui::RoutingControls controls(&client);
        controls.show();
        auto *proxy = controls.findChild<QCheckBox *>("systemProxySwitch");
        QVERIFY(proxy);
        QVERIFY(!proxy->isEnabled());
        QSignalSpy requested(&controls, &ui::RoutingControls::systemProxyRequested);
        controls.setSystemProxyState(false, true);
        QTest::mouseClick(proxy, Qt::LeftButton);
        QCOMPARE(requested.size(), 1);
        QCOMPARE(requested.last().first().toBool(), true);
        controls.setSystemProxyState(false, true);
        QVERIFY(!proxy->isChecked());
        QCOMPARE(requested.size(), 1);
        controls.setSystemProxyState(true, true);
        proxy->setFocus();
        QTest::keyClick(proxy, Qt::Key_Space);
        QCOMPARE(requested.size(), 2);
        QCOMPARE(requested.last().first().toBool(), false);
    }

    void tunSwitchWaitsForConfirmationAndCanTurnOff() {
        RoutingServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        core::MihomoClient client;
        ui::RoutingControls controls(&client);
        controls.show();
        auto *tun = controls.findChild<QCheckBox *>("tunSwitch");
        QSignalSpy applied(&controls, &ui::RoutingControls::tunApplied);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(controls.tunAvailable());
        QTest::mouseClick(tun, Qt::LeftButton);
        QVERIFY(!tun->isChecked());
        QVERIFY(!tun->isEnabled());
        controls.requestTunChange(true);
        QTRY_COMPARE(applied.size(), 1);
        QVERIFY(tun->isChecked());
        QVERIFY(tun->isEnabled());
        QCOMPARE(server.patches, 1);
        QTest::mouseClick(tun, Qt::LeftButton);
        QTRY_COMPARE(applied.size(), 2);
        QVERIFY(!tun->isChecked());
        QCOMPARE(server.patches, 2);
        QCOMPARE(applied.last().first().toBool(), false);
    }

    void knownPermissionFailureDoesNotSendEnableButAllowsDisable() {
        RoutingServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        core::MihomoClient client;
        ui::RoutingControls controls(&client);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(controls.tunAvailable());
        controls.setTunEnableBlockedReason("TUN requires a privileged service.");
        QSignalSpy errors(&controls, &ui::RoutingControls::errorOccurred);
        QSignalSpy applied(&controls, &ui::RoutingControls::tunApplied);
        controls.requestTunChange(true);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(server.patches, 0);
        QCOMPARE(applied.size(), 0);
        QVERIFY(!controls.tunEnabled());
        server.enabled = true;
        client.fetchConfigs();
        QTRY_VERIFY(controls.tunEnabled());
        controls.requestTunChange(false);
        QTRY_COMPARE(applied.size(), 1);
        QCOMPARE(server.patches, 1);
        QVERIFY(!controls.tunEnabled());
        controls.setTunEnableBlockedReason({}); // External/privileged core.
        controls.requestTunChange(true);
        QTRY_COMPARE(applied.size(), 2);
        QVERIFY(controls.tunEnabled());
    }

    void failedDeviceSetupDoesNotEnableOrPersistToggle() {
        RoutingServer server;
        server.rejectDevice = true;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        core::MihomoClient client;
        ui::RoutingControls controls(&client);
        controls.show();
        QSignalSpy applied(&controls, &ui::RoutingControls::tunApplied);
        QSignalSpy errors(&controls, &ui::RoutingControls::errorOccurred);
        client.setEndpoint(server.endpoint());
        QTRY_VERIFY(controls.tunAvailable());
        auto *tun = controls.findChild<QCheckBox *>("tunSwitch");
        QTest::mouseClick(tun, Qt::LeftButton);
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!tun->isChecked());
        QVERIFY(tun->isEnabled());
        QVERIFY(tun->toolTip().contains("permission"));
        QCOMPARE(applied.size(), 0);
    }

    void compactLayoutKeepsBothSwitchesReachable() {
        core::MihomoClient client;
        ui::RoutingControls controls(&client);
        controls.setSystemProxyState(true, true);
        controls.show();
        auto *proxy = controls.findChild<QCheckBox *>("systemProxySwitch");
        auto *tun = controls.findChild<QCheckBox *>("tunSwitch");
        QVERIFY(controls.sizeHint().width() < 300);
        QVERIFY(!proxy->geometry().intersects(tun->geometry()));
        QVERIFY(controls.rect().contains(proxy->geometry()));
        QVERIFY(controls.rect().contains(tun->geometry()));
        QVERIFY(!proxy->accessibleName().isEmpty());
        QVERIFY(!tun->accessibleName().isEmpty());
    }
};

QTEST_MAIN(RoutingControlsTest)
#include "routing_controls_test.moc"
