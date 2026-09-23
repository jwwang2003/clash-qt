// RoutingControls: the toolbar's two switches as a VIEW of RoutingController.
//
// The widget no longer owns any routing state, so the cases drive the
// controller - over the deterministic FakeBackend and over a real
// platform::SystemProxyService whose OS call is substituted through its
// constructor - and assert what the switches then show. Each case protects what
// it protected before: the system-proxy switch mirrors the confirmed state
// without feeding its own render back as intent; the TUN switch waits for the
// controller's read-back and never renders a request as applied; a known
// permission block is refused before anything is sent but never blocks a
// disable; a failed device setup leaves the switch off, re-enabled and
// explained.
//
// The error assertions moved to RoutingController::errorOccurred, which is now
// the single error channel for routing. The widget deliberately does not
// republish it - it only reads lastError() for the tooltip - so one failure is
// reported exactly once.

#include <QtTest>
#include <QCheckBox>

#include <atomic>
#include <memory>

#include "app/runtime/routing_controller.h"
#include "platform/proxy/system_proxy_service.h"
#include "support/backend/fake_backend.h"
#include "ui/shell/routing_controls.h"
#include "ui/theme/theme.h"

namespace cb = core::backend;

using app::runtime::RestoreDelays;
using app::runtime::RoutingController;
using testsupport::backend::FakeBackend;
using testsupport::backend::Gate;
using testsupport::backend::RequestKind;
using testsupport::backend::RequestOutcome;

namespace {

using Action = platform::SystemProxyAction;
using Config = platform::ProxyConfig;
using Result = platform::SystemProxyResult;

// The grace window compressed so a disconnect's restore cannot outlive a case.
constexpr RestoreDelays kFastDelays{0, 30};

struct ProxyCalls {
    std::atomic_int enables{0};
    std::atomic_int disables{0};
    std::atomic_int refreshes{0};
    std::atomic_int restores{0};
    int changes() const { return enables.load() + disables.load(); }
};

// A real service whose only OS call is this in-process function: nothing
// touches the machine's proxy settings.
std::unique_ptr<platform::SystemProxyService> makeService(std::shared_ptr<ProxyCalls> calls,
                                                          std::shared_ptr<Config> osState) {
    return std::make_unique<platform::SystemProxyService>(
        nullptr, [calls, osState](Action action, const Config &config) {
            Result result;
            result.state.supported = true;
            result.state.valid = true;
            switch (action) {
                case Action::Enable:
                    ++calls->enables;
                    *osState = config;
                    result.state.owned = true;
                    break;
                case Action::Disable:
                    ++calls->disables;
                    *osState = Config{};
                    break;
                case Action::Restore:
                    ++calls->restores;
                    *osState = Config{};
                    break;
                case Action::Refresh:
                    ++calls->refreshes;
                    break;
            }
            result.state.config = *osState;
            result.state.owned = result.state.config.port != 0;
            return result;
        });
}

cb::Endpoint endpointAt(quint16 port) {
    cb::Endpoint endpoint;
    endpoint.host = QStringLiteral("127.0.0.1");
    endpoint.port = port;
    return endpoint;
}

// Attach and settle, so the controller has a connection and a configuration.
void connectWithConfig(FakeBackend &backend, bool tunEnabled) {
    backend.staged().config.tunEnabled = tunEnabled;
    backend.attach(endpointAt(29190));
    QVERIFY(backend.flushEvents());
}

cb::RequestId pendingOfKind(const FakeBackend &backend, RequestKind kind) {
    for (const cb::RequestId id : backend.pendingRequests())
        if (backend.pendingKind(id) == kind) return id;
    return cb::RequestId::Invalid;
}

}  // namespace

class RoutingControlsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { ui::theme::install(); }

    void systemProxySwitchMirrorsActualStateWithoutFeedback() {
        auto calls = std::make_shared<ProxyCalls>();
        auto osState = std::make_shared<Config>();
        auto service = makeService(calls, osState);
        FakeBackend backend;
        RoutingController routing(backend, service.get(), kFastDelays);
        ui::RoutingControls controls(&routing);
        controls.show();
        auto *proxy = controls.findChild<QCheckBox *>("systemProxySwitch");
        QVERIFY(proxy);
        // Nothing read back yet: the switch is a view, so it offers nothing.
        QVERIFY(!proxy->isEnabled());

        Config target;
        target.port = 27890;
        routing.setProxyTarget(target);
        connectWithConfig(backend, false);
        routing.refreshSystemProxy();
        QTRY_VERIFY(proxy->isEnabled());
        QVERIFY(!proxy->isChecked());

        QTest::mouseClick(proxy, Qt::LeftButton);
        QTRY_COMPARE(calls->enables.load(), 1);
        QTRY_VERIFY(proxy->isChecked());
        QCOMPARE(calls->changes(), 1);

        // Rendering a state the OS reports must never come back as intent.
        *osState = Config{};
        routing.refreshSystemProxy();
        QTRY_VERIFY(!proxy->isChecked());
        QCOMPARE(calls->changes(), 1);

        // …and the keyboard path is the same intent as the pointer one.
        proxy->setFocus();
        QTest::keyClick(proxy, Qt::Key_Space);
        QTRY_COMPARE(calls->enables.load(), 2);
        QTRY_VERIFY(proxy->isChecked());
        proxy->setFocus();
        QTest::keyClick(proxy, Qt::Key_Space);
        QTRY_COMPARE(calls->disables.load(), 1);
        QTRY_VERIFY(!proxy->isChecked());
    }

    void tunSwitchWaitsForConfirmationAndCanTurnOff() {
        FakeBackend backend;
        RoutingController routing(backend, nullptr, kFastDelays);
        ui::RoutingControls controls(&routing);
        controls.show();
        auto *tun = controls.findChild<QCheckBox *>("tunSwitch");
        QSignalSpy applied(&routing, &RoutingController::tunConfirmed);
        connectWithConfig(backend, false);
        QTRY_VERIFY(controls.tunAvailable());
        backend.setRequestGate(Gate::Held);

        QTest::mouseClick(tun, Qt::LeftButton);
        // A request in flight is NOT an applied change.
        QVERIFY(!tun->isChecked());
        QVERIFY(!tun->isEnabled());
        controls.requestTunChange(true);  // coalesced away: one change at a time
        QCOMPARE(backend.issuedCount(RequestKind::SetTun), 1);

        backend.staged().tunActual = true;
        QVERIFY(backend.releaseRequest(pendingOfKind(backend, RequestKind::SetTun)));
        QTRY_COMPARE(applied.size(), 1);
        QVERIFY(tun->isChecked());
        QVERIFY(tun->isEnabled());
        QCOMPARE(backend.issuedCount(RequestKind::SetTun), 1);

        QTest::mouseClick(tun, Qt::LeftButton);
        backend.staged().tunActual = false;
        QVERIFY(backend.releaseRequest(pendingOfKind(backend, RequestKind::SetTun)));
        QTRY_COMPARE(applied.size(), 2);
        QVERIFY(!tun->isChecked());
        QCOMPARE(backend.issuedCount(RequestKind::SetTun), 2);
        QCOMPARE(applied.last().first().toBool(), false);
    }

    void knownPermissionFailureDoesNotSendEnableButAllowsDisable() {
        FakeBackend backend;
        RoutingController routing(backend, nullptr, kFastDelays);
        ui::RoutingControls controls(&routing);
        connectWithConfig(backend, false);
        QTRY_VERIFY(controls.tunAvailable());
        controls.setTunEnableBlockedReason("TUN requires a privileged service.");
        QSignalSpy errors(&routing, &RoutingController::errorOccurred);
        QSignalSpy applied(&routing, &RoutingController::tunConfirmed);
        controls.requestTunChange(true);
        QCOMPARE(errors.size(), 1);
        QCOMPARE(backend.issuedCount(RequestKind::SetTun), 0);
        QCOMPARE(applied.size(), 0);
        QVERIFY(!controls.tunEnabled());

        // The core reports TUN on anyway; turning it OFF must stay possible.
        backend.staged().config.tunEnabled = true;
        backend.refreshConfig();
        QVERIFY(backend.flushEvents());
        QTRY_VERIFY(controls.tunEnabled());
        backend.staged().tunActual = false;
        controls.requestTunChange(false);
        QTRY_COMPARE(applied.size(), 1);
        QCOMPARE(backend.issuedCount(RequestKind::SetTun), 1);
        QVERIFY(!controls.tunEnabled());

        controls.setTunEnableBlockedReason({});  // External/privileged core.
        backend.staged().tunActual = true;
        controls.requestTunChange(true);
        QTRY_COMPARE(applied.size(), 2);
        QVERIFY(controls.tunEnabled());
    }

    void failedDeviceSetupDoesNotEnableOrPersistToggle() {
        FakeBackend backend;
        RoutingController routing(backend, nullptr, kFastDelays);
        ui::RoutingControls controls(&routing);
        controls.show();
        QSignalSpy applied(&routing, &RoutingController::tunConfirmed);
        QSignalSpy errors(&routing, &RoutingController::errorOccurred);
        connectWithConfig(backend, false);
        QTRY_VERIFY(controls.tunAvailable());
        backend.setRequestGate(Gate::Held);
        auto *tun = controls.findChild<QCheckBox *>("tunSwitch");
        QTest::mouseClick(tun, Qt::LeftButton);
        // The controller accepted the PATCH and read TUN back as still off.
        backend.staged().tunActual = false;
        QVERIFY(backend.releaseRequest(
            pendingOfKind(backend, RequestKind::SetTun), RequestOutcome::Failure,
            {cb::ErrorCode::Protocol,
             QStringLiteral("The controller accepted the request, but TUN is still disabled. "
                            "Check the core logs and whether the core has permission to create "
                            "a TUN interface.")}));
        QTRY_COMPARE(errors.size(), 1);
        QVERIFY(!tun->isChecked());
        QVERIFY(tun->isEnabled());
        QVERIFY(tun->toolTip().contains("permission"));
        QCOMPARE(applied.size(), 0);
    }

    void compactLayoutKeepsBothSwitchesReachable() {
        FakeBackend backend;
        RoutingController routing(backend, nullptr, kFastDelays);
        ui::RoutingControls controls(&routing);
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
