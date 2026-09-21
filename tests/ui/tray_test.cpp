// Tray surfaces: the shell-command clipboard entries and the proxy-group menu.
//
// The proxy menu now consumes core::backend::BackendBridge over the
// deterministic FakeBackend instead of core::MihomoClient over a loopback HTTP
// server. What the menu owes is unchanged and is asserted unchanged: it renders
// only the snapshot it was last given, escapes '&' for the menu without
// mangling the name it sends back, and refuses a choice that the newest
// snapshot no longer contains. The former PUT-URL and Authorization-header
// assertions were assertions about the controller transport, not about this
// widget; controller-tests owns them.

#include <QtTest>
#include <QMenu>

#include "core/backend/backend_bridge.h"
#include "support/backend/fake_backend.h"
#include "ui/shell/proxy_environment.h"
#include "ui/shell/tray_proxy_menu.h"

namespace cb = core::backend;

using testsupport::backend::FakeBackend;
using testsupport::backend::RequestKind;

namespace {

cb::Endpoint endpointAt(quint16 port) {
    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = port;
    endpoint.secret = QStringLiteral("test-auth");
    return endpoint;
}

// One snapshot, published the way the backend publishes one.
void publishProxies(FakeBackend &backend, const QVector<cb::ProxyGroup> &groups) {
    backend.staged().groups = groups;
    backend.refreshProxies();
    QVERIFY(backend.flushEvents());
}

}  // namespace

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
        FakeBackend backend;
        cb::BackendBridge bridge(backend);
        ui::TrayProxyMenu menu(&bridge);
        QSignalSpy selected(&bridge, &cb::BackendBridge::nodeSelected);
        backend.attach(endpointAt(29099));
        QVERIFY(backend.flushEvents());
        QTRY_VERIFY(bridge.isConnected());
        const cb::ProxyGroup group{"A&B / group", "Selector", "DIRECT", {"DIRECT", "Tokyo & Seoul"}};
        publishProxies(backend, {group});
        QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
        QCOMPARE(menu.actions().size(), 1);
        auto *submenu = menu.actions().first()->menu();
        QVERIFY(submenu);
        QCOMPARE(submenu->title(), QString("A&&B / group"));
        QCOMPARE(submenu->actions().at(1)->text(), QString("Tokyo && Seoul"));
        QVERIFY(submenu->actions().first()->isChecked());
        submenu->actions().at(1)->trigger();
        QTRY_COMPARE(selected.size(), 1);
        // The escaping is the menu's, never the intent's: the group and node go
        // back exactly as the snapshot spelled them.
        QCOMPARE(selected.last().at(0).toString(), QString("A&B / group"));
        QCOMPARE(selected.last().at(1).toString(), QString("Tokyo & Seoul"));
        QCOMPARE(backend.issuedCount(RequestKind::SelectNode), 1);
        publishProxies(backend, {group});
        QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
        auto *staleAction = menu.actions().first()->menu()->actions().at(1);
        publishProxies(backend, {});
        const int before = backend.issuedCount(RequestKind::SelectNode);
        staleAction->trigger();
        QVERIFY(backend.flushEvents());
        QCOMPARE(backend.issuedCount(RequestKind::SelectNode), before);
        QCOMPARE(selected.size(), 1);
        QVERIFY(QMetaObject::invokeMethod(&menu, "aboutToShow"));
        QCOMPARE(menu.actions().size(), 1);
        QVERIFY(!menu.actions().first()->isEnabled());
        QVERIFY(menu.findChildren<QMenu *>().isEmpty());
    }

    void automaticAndReadOnlyGroupsHaveCorrectActions() {
        FakeBackend backend;
        cb::BackendBridge bridge(backend);
        ui::TrayProxyMenu menu(&bridge);
        publishProxies(backend, {
            {"Automatic", "URLTest", "DIRECT", {"DIRECT"}, {}},
            {"Balance", "LoadBalance", {}, {"DIRECT"}, {}}
        });
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
