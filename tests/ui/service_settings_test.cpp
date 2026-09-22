// ServiceSettings: the uninstall guard, asserted.
//
// WHY THIS SUITE EXISTS. ServiceSettings::serviceRunning_ answers one question -
// "is a core already running under the privileged helper, possibly for another
// app session?" - and three things depend on the answer: the confirmation that
// gates the uninstall, the `available` predicate behind Install/Repair, Remove
// and the mode checkbox, and the notice that explains why they are inert.
//
// Decision D3 removed this page's second platform::PrivilegedServiceClient,
// which was the ONLY producer of that flag, and left all three consumers in
// place. The flag was initialised false and written nowhere, so the guard was
// permanently open: the privileged helper could be uninstalled out from under a
// core another session was running. Nothing failed, because nothing asserted it.
//
// So these cases assert the flag's EFFECT at each of the three sites, driven
// through the published contract - core::backend::PrivilegedServiceStatus
// carries the helper's own running-core report, BackendBridge republishes it,
// this page consumes it. Break any link and this suite fails.
//
// WHAT IT DELIBERATELY DOES NOT DO: click Remove. This machine may have a real
// helper installed, and platform::PrivilegedServiceInstaller::uninstall() would
// remove it for real - so a regression would be "proved" by damaging the host.
// The guard is therefore read through ServiceSettings::canChangeInstallation(),
// the single predicate the confirmation handler itself calls. Nothing here
// installs, uninstalls, restarts or connects to the helper; the only privileged
// code that runs is the installer's read-only lstat() probe.

#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>

#include <memory>

#include "app/app_context.h"
#include "core/backend/backend_bridge.h"
#include "core/profiles/profile_store.h"
#include "platform/service/privileged_service_installer.h"
#include "support/backend/fake_backend.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"
#include "ui/pages/settings/service_settings.h"

namespace cb = core::backend;

using testsupport::backend::FakeBackend;

namespace {

// The exact string the page shows at the third call site. Spelled out here so a
// silent rewording cannot turn this assertion into a tautology.
const char *kRunningNotice =
    "A service core is still running. Stop the app session that owns it, then check status.";

// One page over one deterministic backend, with the real ProfileStore the page
// connects to. Everything is stack-owned and torn down per case.
struct Harness {
    FakeBackend backend;
    cb::BackendBridge bridge{backend};
    core::ProfileStore profiles;
    app::Context context;
    std::unique_ptr<ui::ServiceSettings> page;

    Harness() {
        backend.setServiceSupported(true);
        context.profiles = &profiles;
        page = std::make_unique<ui::ServiceSettings>(context, &bridge);
        settle();
    }

    // The page asks for status from its own constructor through a zero-timer,
    // and the installer answers its read-only probe off-thread. flushEvents()
    // returns at once while the backend queue is still empty, so the loop is
    // what actually lets those land before a case starts asserting.
    void settle() {
        for (int pass = 0; pass < 4; ++pass) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            backend.flushEvents();
        }
    }

    // The full published path: the backend's struct -> the observer -> the
    // bridge signal -> the page. Not a direct signal emission, so a field
    // dropped anywhere along it shows up here.
    void publishHelperStatus(bool coreRunning) {
        cb::PrivilegedServiceStatus status;
        status.state = cb::ServiceState::Connected;
        status.version = QStringLiteral("helper-1.2.3");
        status.coreRunning = coreRunning;
        backend.setServiceStatus(status);
        bridge.requestPrivilegedServiceStatus();
        QVERIFY(backend.flushEvents());
        settle();
    }

    QPushButton *install() const {
        return page->findChild<QPushButton *>(QStringLiteral("installPrivilegedService"));
    }
    QPushButton *remove() const {
        return page->findChild<QPushButton *>(QStringLiteral("removePrivilegedService"));
    }
    QCheckBox *modeBox() const {
        return page->findChild<QCheckBox *>(QStringLiteral("usePrivilegedService"));
    }
    QString notice() const {
        auto *banner = page->findChild<QLabel *>(QStringLiteral("noticeBanner"));
        return banner ? banner->text() : QString();
    }
};

}  // namespace

class ServiceSettingsTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("svcset"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        // Not a soft skip: on a platform with no privileged helper all three
        // call sites are gated on `supported` first, so there is no guard to
        // assert. Say so out loud rather than reporting a vacuous pass.
        if (!platform::PrivilegedServiceInstaller::isSupported())
            QSKIP("no privileged service on this platform: the uninstall guard does not exist here");
    }
    void cleanupTestCase() {
        if (!environment_) return;
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    // Call sites 1 and 2: the uninstall precondition and `available`.
    void aRunningServiceCoreBlocksUninstallAndDisablesTheControls() {
        Harness h;
        QVERIFY(h.install());
        QVERIFY(h.remove());
        QVERIFY(h.modeBox());

        // Baseline: helper reachable, no core under it, our core stopped.
        h.publishHelperStatus(false);
        QVERIFY2(h.page->canChangeInstallation(),
                 "nothing was running, yet the page refused to install or remove");
        QVERIFY2(h.install()->isEnabled(), "Install/Repair was disabled with no core running");

        // A core is running under the helper - ours is still stopped, so this is
        // another app session's. Uninstalling now would pull the helper out from
        // under it.
        h.publishHelperStatus(true);
        QVERIFY2(!h.page->canChangeInstallation(),
                 "the helper reported a running core and the uninstall guard still let it through");
        QVERIFY2(!h.install()->isEnabled(),
                 "Install/Repair stayed enabled while a service core was running");
        QVERIFY2(!h.remove()->isEnabled(),
                 "Remove stayed enabled while a service core was running");
        QVERIFY2(!h.modeBox()->isEnabled(),
                 "the mode checkbox stayed enabled while a service core was running");

        // And it re-arms: a guard that latches shut is its own defect.
        h.publishHelperStatus(false);
        QVERIFY2(h.page->canChangeInstallation(),
                 "the guard stayed shut after the service core stopped");
        QVERIFY2(h.install()->isEnabled(), "Install/Repair stayed disabled after the core stopped");
    }

    // Call site 3: the notice that says why the controls are inert.
    void aRunningServiceCoreIsExplainedInTheNotice() {
        Harness h;
        h.publishHelperStatus(false);
        QVERIFY2(h.notice() != QString::fromUtf8(kRunningNotice),
                 "the running-core notice showed with no core running");

        h.publishHelperStatus(true);
        QCOMPARE(h.notice(), QString::fromUtf8(kRunningNotice));
    }

    // Not hearing back is not evidence that nothing is running. A query that
    // fails carries coreRunning == false for want of an answer, and taking that
    // at face value would disarm an armed guard.
    void anUnansweredStatusQueryDoesNotDisarmTheGuard() {
        Harness h;
        h.publishHelperStatus(true);
        QVERIFY(!h.page->canChangeInstallation());

        emit h.bridge.privilegedServiceStatus(cb::ServiceState::Failed, QString(),
                                              QStringLiteral("the service disconnected"), false);
        QCoreApplication::processEvents();
        QVERIFY2(!h.page->canChangeInstallation(),
                 "a failed status query disarmed the uninstall guard");
        QCOMPARE(h.notice(), QString::fromUtf8(kRunningNotice));
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(ServiceSettingsTest)
#include "service_settings_test.moc"
