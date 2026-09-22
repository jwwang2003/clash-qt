#include "contracts/backend/common/backend_common_cases.h"

#include <vector>

#include <QtTest>

#include "core/backend/backend.h"
#include "support/backend/common_contract_driver.h"

namespace testsupport::backend::common::cases {

namespace {

namespace cb = ::core::backend;
using Observer = ContractObserver;

/// A distinctive payload: Latin-1 accents, characters outside the basic
/// multilingual plane's ASCII range, and a digit run. A boundary that
/// re-encoded, truncated at a NUL or narrowed to Latin-1 would not return this
/// unchanged. No control character and no quote, so it is also a legal JSON
/// string body for the loopback controller.
QString distinctiveText(const QString &tag) {
    return QStringLiteral("1.19.31-%1-ünïcödé-版本-0123456789").arg(tag);
}

/// Attaches controller `index` and waits until the controller has answered.
/// `connected` is what proves the controller is THERE - readiness and
/// connectivity are answers from the controller, not flags the caller sets.
bool attachAndConnect(BackendDriver &driver, int index) {
    driver.backend().attach(driver.controller(index));
    return driver.waitUntil([&driver] { return driver.backend().isConnected(); });
}

/// Brings a managed core up from the named slot, through the published facade:
/// validate, launch, answer GET /version. Readiness is the controller's 200
/// plus a string `version`, never "the process started". Returns the start
/// request, or Invalid with `why` explaining which step did not happen.
cb::RequestId bringUpManagedCore(BackendDriver &driver, const QString &slot, QString *why) {
    if (!driver.prepareValidation(true)) {
        *why = QStringLiteral("the validation outcome could not be staged: %1")
                   .arg(driver.errorString());
        return cb::RequestId::Invalid;
    }
    const cb::RequestId launch = driver.startManagedCore(slot);
    if (launch == cb::RequestId::Invalid) {
        *why = QStringLiteral("start() was rejected: %1").arg(driver.errorString());
        return launch;
    }
    if (!driver.settleValidation()) {
        *why = QStringLiteral("the candidate's validation never settled. %1")
                   .arg(driver.describe());
        return cb::RequestId::Invalid;
    }
    if (!driver.driveToRunning()) {
        *why = QStringLiteral("the core never answered /version with a string version. %1")
                   .arg(driver.describe());
        return cb::RequestId::Invalid;
    }
    return launch;
}

}  // namespace

// ------------------------------------------------- identity and the budget

void identityAndPublishedBudget(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();

    QCOMPARE(backend.generation(), cb::Generation::Initial);

    const cb::BackendIdentity identity = backend.identity();
    QVERIFY2(!identity.name.isEmpty(), "a backend that will not say what it is");
    QCOMPARE(identity.interfaceRevision, 1u);

    // Section 3 publishes these as DATA because a consumer may not assume a
    // fixed timeout, and they must be the budget the backend will really
    // apply - not a constant a module consumer is told instead of the truth.
    const cb::BackendTimings timings = backend.timings();
    QCOMPARE(timings.idleDeadlineMs, cb::kContractTimings.idleDeadlineMs);
    QCOMPARE(timings.serviceIdleDeadlineMs, cb::kContractTimings.serviceIdleDeadlineMs);
    QCOMPARE(timings.hardCapMs, cb::kContractTimings.hardCapMs);
    QCOMPARE(timings.probeIntervalMs, cb::kContractTimings.probeIntervalMs);
    QCOMPARE(timings.probeTimeoutMs, cb::kContractTimings.probeTimeoutMs);
    QCOMPARE(timings.terminateWaitMs, cb::kContractTimings.terminateWaitMs);

    const cb::FeatureSet features = backend.features();
    QVERIFY(features.has(cb::Feature::ManagedLifecycle));
    QVERIFY(features.has(cb::Feature::ConfirmedTunChange));
    QVERIFY(features.has(cb::Feature::ConfigValidation));
    QVERIFY(features.has(cb::Feature::Providers));
    QVERIFY(features.has(cb::Feature::DnsQuery));
    QVERIFY(features.has(cb::Feature::DnsCacheFlush));
    QVERIFY(features.has(cb::Feature::GeoDatabaseUpdate));
    QVERIFY(features.has(cb::Feature::MemoryStream));

    // At rest, and every one of these is an instance answer rather than a
    // process-global static (section 9).
    QCOMPARE(backend.state(), cb::CoreState::Stopped);
    QCOMPARE(backend.ownership(), cb::Ownership::None);
    QVERIFY(!backend.isConnected());
    QVERIFY(!backend.isExternalControllerConnected());
    QVERIFY(!cb::isValid(backend.managedEndpoint()));

    // G-ATTACHED-AT-REST, closed. It was an OPEN GATE here: the two real rows
    // answered Attached for a controller nothing had pointed them at, because
    // core::Endpoint defaults to the DISCOVERY default 127.0.0.1:9090 and
    // MihomoBackendImpl::isAttached() is `client_.endpoint().isValid()`.
    // MihomoClient now starts from MihomoClient::detachedEndpoint() instead, so
    // all four implementations answer the section 1 definition of Attached -
    // "any endpoint the user or discovery pointed us at" - identically.
    // Discovery still hands the 127.0.0.1:9090 default out; a guess about where
    // a controller might be is not an attachment until attach() accepts it.
    QVERIFY2(backend.attachmentOwnership() == cb::Ownership::None,
             "a backend at rest reports an attachment nothing made: Ownership::None is "
             "'nothing is attached' (section 1), and the discovery default is a guess "
             "rather than an attachment");
    QVERIFY2(!backend.isAttached(),
             "a backend at rest reports itself attached to a controller nothing pointed it at");
    QVERIFY(backend.activeConfigPaths().isEmpty());
    QVERIFY(!backend.isRestartPending());
    QVERIFY(!backend.isManagedCoreActive());
    QVERIFY(!backend.isTunChangePending());
    QVERIFY(!backend.isProviderBusy());
    QCOMPARE(backend.executionMode(), cb::ExecutionMode::Managed);
    QVERIFY(!backend.usesPrivilegedService());
}

// ------------------------------------------- request identity and payloads

void requestIdentityAndLosslessPayloads(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    const QString version = distinctiveText(QStringLiteral("version"));
    QVERIFY(driver.stageVersion(0, version));
    QVERIFY(attachAndConnect(driver, 0));

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));

    // Nothing here bumps the generation, so the submit-time stamp every
    // completion must carry (A1) is this value. A backend that stamped at
    // DELIVERY would also pass a comparison against itself - which is why A1
    // says that stamping is self-defeating - but it cannot pass this one, and
    // the superseded case pins the other end.
    const cb::Generation submitted = backend.generation();

    const cb::RequestId first = backend.refreshVersion();
    const cb::RequestId second = backend.refreshVersion();
    const cb::RequestId proxies = backend.refreshProxies();
    const cb::RequestId rules = backend.refreshRules();
    const cb::RequestId config = backend.refreshConfig();
    const cb::RequestId mode = backend.setMode(QStringLiteral("global"));

    const std::vector<cb::RequestId> ids{first, second, proxies, rules, config, mode};
    for (const cb::RequestId id : ids) {
        QVERIFY2(id != cb::RequestId::Invalid, "a mutating call rejected an ordinary request");
        QVERIFY2(cb::number(id) > 0, "RequestId::Invalid is the only zero");
    }
    for (std::size_t outer = 0; outer < ids.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < ids.size(); ++inner) {
            QVERIFY2(ids[outer] != ids[inner],
                     "two outstanding operations were given the same RequestId");
        }
    }

    QVERIFY2(driver.waitUntil([&] {
                 return observer.find(QStringLiteral("version"), second) != nullptr &&
                        observer.find(QStringLiteral("proxies"), proxies) != nullptr &&
                        observer.find(QStringLiteral("rules"), rules) != nullptr &&
                        observer.find(QStringLiteral("config"), config) != nullptr &&
                        observer.find(QStringLiteral("mode"), mode) != nullptr;
             }),
             "a completion never arrived for a request the backend accepted");

    // Every completion carries back the id it completes, on its own channel.
    const Observer::Record *versionRecord = observer.find(QStringLiteral("version"), first);
    QVERIFY(versionRecord != nullptr);
    QCOMPARE(versionRecord->completion.request, first);
    QCOMPARE(versionRecord->completion.status, cb::CompletionStatus::Ok);
    QVERIFY2(versionRecord->completion.generation == submitted,
             "a completion was not stamped with the generation its request was submitted "
             "under (backend-r2 A1)");
    QVERIFY2(versionRecord->payload == version,
             qPrintable(QStringLiteral("the version payload did not survive: %1")
                            .arg(versionRecord->payload)));
    // The same value twice, so a one-shot buffer cannot pass by accident.
    const Observer::Record *repeat = observer.find(QStringLiteral("version"), second);
    QVERIFY(repeat != nullptr);
    QCOMPARE(repeat->payload, version);
    QVERIFY(repeat->completion.request != versionRecord->completion.request);

    QCOMPARE(observer.find(QStringLiteral("mode"), mode)->payload, QStringLiteral("global"));
    QVERIFY(observer.find(QStringLiteral("rules"), rules)->completion.isOk());
    QVERIFY(observer.find(QStringLiteral("config"), config)->completion.isOk());
    QVERIFY(observer.find(QStringLiteral("proxies"), proxies)->completion.isOk());

    QVERIFY(backend.removeObserver(&observer));
}

// --------------------------------------------------- observer registration

void observerRegistrationIsExplicit(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    Observer first(&driver, QStringLiteral("first"));
    Observer removed(&driver, QStringLiteral("removed"));

    QVERIFY2(!backend.addObserver(nullptr), "a null observer was registered");
    QVERIFY(backend.addObserver(&first));
    QVERIFY2(!backend.addObserver(&first), "the same observer was registered twice");
    QVERIFY(backend.addObserver(&removed));
    QVERIFY2(!backend.removeObserver(nullptr), "removing null answered true");
    QVERIFY(backend.removeObserver(&removed));
    QVERIFY2(!backend.removeObserver(&removed), "removing an unregistered observer answered true");

    QVERIFY(attachAndConnect(driver, 0));
    QVERIFY(driver.waitUntil([&] { return first.callbackCount > 3; }));
    QVERIFY(driver.deliver());
    QVERIFY2(removed.callbackCount == 0, "an observer removed before delivery was invoked");
    QVERIFY(backend.removeObserver(&first));
}

void removingAnObserverDuringDeliveryStopsFurtherCallbacks(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    Observer first(&driver, QStringLiteral("first"));
    Observer second(&driver, QStringLiteral("second"));
    Observer third(&driver, QStringLiteral("third"));
    QVERIFY(backend.addObserver(&first));
    QVERIFY(backend.addObserver(&second));
    QVERIFY(backend.addObserver(&third));

    // The second observer removes itself AND the third from inside its first
    // callback. The third has not been reached yet in this delivery, and it
    // must not be reached at all.
    second.onNextEvent = [&] {
        backend.removeObserver(&second);
        backend.removeObserver(&third);
    };

    QVERIFY(attachAndConnect(driver, 0));
    QVERIFY(driver.waitUntil([&] { return first.callbackCount > 3; }));
    QVERIFY(driver.deliver());

    QCOMPARE(second.callbackCount, 1);
    QVERIFY2(third.callbackCount == 0,
             "an observer removed during delivery was invoked afterwards");
    QVERIFY2(!backend.removeObserver(&second), "the self-removal did not take effect");
    QVERIFY(backend.removeObserver(&first));
}

void anObserverAddedDuringACallbackSeesOnlyLaterEvents(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    Observer first(&driver, QStringLiteral("first"));
    Observer late(&driver, QStringLiteral("late"));
    QVERIFY(backend.addObserver(&first));
    first.onNextEvent = [&] { QVERIFY(backend.addObserver(&late)); };

    QVERIFY(attachAndConnect(driver, 0));
    QVERIFY(driver.waitUntil([&] { return first.callbackCount > 3; }));
    QVERIFY(driver.deliver());

    QVERIFY2(first.callbackCount > late.callbackCount,
             "an observer added during delivery saw everything the earlier one did");
    QVERIFY2(!late.sawReentrantDelivery,
             "an observer added inside a callback was then invoked re-entrantly");
    QVERIFY(backend.removeObserver(&first));
    QVERIFY(backend.removeObserver(&late));
}

void anObserverAddedBeforeDeliverySeesNoEarlierEvents(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    Observer first(&driver, QStringLiteral("first"));
    QVERIFY(backend.addObserver(&first));

    // attach() PRODUCES the endpoint change and the empty live view from
    // inside the call; section 7 rule 1 defers delivering them until after it
    // returns. `late` registers in that window.
    backend.attach(driver.controller(0));
    Observer late(&driver, QStringLiteral("late"));
    QVERIFY(backend.addObserver(&late));

    QVERIFY(driver.waitUntil([&] { return first.liveStateClears >= 1 && !first.endpoints.empty(); }));
    QVERIFY(driver.deliver());

    // Only the two event kinds attach() itself produced are asserted on.
    // Anything the controller answers afterwards is produced AFTER the
    // registration and may legitimately reach `late`.
    //
    // G-MODULE-OBSERVER-WATERMARK, closed. These two rows were the minimal
    // reproducer: ModuleBackend kept its observers in a plain vector with no
    // registration watermark, while both in-process implementations stamped
    // each observer with the event sequence it joined at, so an observer that
    // registered between production and delivery received events produced
    // before it existed. The host shim cannot see when the MODULE produced an
    // event, only when the module notified it of one, which is why this was a
    // boundary question rather than a transcription slip. The ABI admission
    // watermark now answers it, and the rows are asserted, not expected to
    // fail: with the fixed ABI in place they XPASSed, which is what took the
    // markers out.
    QVERIFY2(late.endpoints.empty(),
             "an observer registered after the attach that produced it still received the "
             "endpoint change (backend-r4 section 7 rule 3: an observer added during "
             "delivery sees only events produced after it was added)");
    QVERIFY2(late.liveStateClears == 0,
             "an observer registered after the attach that produced them still received the "
             "cleared live view");
    QVERIFY(backend.removeObserver(&first));
    QVERIFY(backend.removeObserver(&late));
}

// -------------------------------------------------- non-re-entrant delivery

void deliveryIsNeverReentrant(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));

    backend.attach(driver.controller(0));
    QVERIFY2(!observer.sawReentrantDelivery, "attach() delivered from inside itself");
    QVERIFY(driver.waitUntil([&] { return backend.isConnected(); }));

    // Every mutating path that produces events, including the one that clears
    // the live view - five synchronous emissions in the code this facade
    // wraps, which is the hazard section 7 exists to stop.
    backend.setMode(QStringLiteral("global"));
    backend.refreshVersion();
    backend.refreshProxies();
    backend.refreshRules();
    backend.refreshConfig();
    backend.requestPrivilegedServiceStatus();
    backend.attach(driver.controller(1));
    backend.detach();
    QVERIFY(driver.deliver());
    QVERIFY(driver.waitUntil([&] { return observer.callbackCount > 12; }));
    QVERIFY(driver.deliver());

    QVERIFY2(!observer.sawReentrantDelivery, "an observer was invoked from inside a mutating call");
    QVERIFY2(observer.callbackCount > 12,
             "the observer received almost nothing: the assertion above is vacuous");
    QVERIFY2(observer.liveStateClears >= 2, "live state was never cleared");

    // And a mutation issued FROM a callback is also delivered later.
    observer.onNextEvent = [&] { backend.refreshVersion(); };
    QVERIFY(attachAndConnect(driver, 0));
    QVERIFY(driver.waitUntil([&] {
        return observer.countOf(QStringLiteral("version"), cb::CompletionStatus::Ok) > 0;
    }));
    QVERIFY2(!observer.sawReentrantDelivery,
             "a mutation issued from inside a callback delivered re-entrantly");
    QVERIFY(backend.removeObserver(&observer));
}

// ---------------------------------------------------------- supersession

void anAbandonedCompletionIsMarkedSuperseded(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    const QString stale = distinctiveText(QStringLiteral("stale"));
    QVERIFY(driver.stageVersion(0, stale));
    QVERIFY(attachAndConnect(driver, 0));

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));
    QVERIFY2(driver.hold(Channel::Version), "the version channel could not be parked");

    const cb::Generation submitted = backend.generation();
    const cb::RequestId version = backend.refreshVersion();
    QVERIFY(version != cb::RequestId::Invalid);

    // The controller changes underneath the outstanding request. Section 2
    // requires the bump BEFORE the abort, because a completion the abort
    // delivers synchronously would otherwise be stamped with the very
    // generation it was meant to invalidate.
    backend.attach(driver.controller(1));
    QVERIFY2(backend.generation() > submitted, "an endpoint change did not bump the generation");

    QVERIFY2(driver.waitUntil([&] {
                 return observer.find(QStringLiteral("version"), version) != nullptr &&
                        observer.lastObserved() > submitted;
             }),
             "the abandoned completion never arrived");

    const Observer::Record *record = observer.find(QStringLiteral("version"), version);
    QVERIFY(record != nullptr);
    QVERIFY2(record->completion.status == cb::CompletionStatus::Superseded,
             "an abandoned completion was not MARKED superseded (backend-r2 A2)");
    QVERIFY2(record->completion.generation == submitted,
             "the abandoned completion was not stamped with its submit generation (A1)");
    QVERIFY2(cb::isSuperseded(record->completion.generation, observer.lastObserved()),
             "the abandoned completion did not compare older than the newest observed "
             "generation: the consumer's second line of defence is inoperative");
    QVERIFY2(!record->admitted, "a conforming consumer would have acted on abandoned work");
    QVERIFY2(record->payload.isEmpty(),
             qPrintable(QStringLiteral("a completion that is not Ok carried a payload (%1): "
                                       "stale data was delivered as live")
                            .arg(record->payload)));
    QVERIFY(driver.release(Channel::Version));
    QVERIFY(backend.removeObserver(&observer));
}

void aNonEndpointBumpReissuesTheSnapshotSet(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    QVERIFY(driver.stageSnapshot());
    QVERIFY(attachAndConnect(driver, 0));

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));
    const cb::Generation before = backend.generation();

    // A managed stop bumps the generation and is NOT an endpoint change. The
    // application polls /version and /proxies every five seconds but never
    // re-issues /rules or /configs, so without the re-issue the rules list and
    // the BaseConfig would stay stale until the next endpoint change
    // (backend-r2's obligation on the single global generation, r3 B3).
    backend.stop();
    QVERIFY2(backend.generation() > before, "a managed stop did not bump the generation");
    const cb::Generation after = backend.generation();

    // The controller's OWN data has to come back, not merely an event: a
    // cleared view carries an empty rule set and an empty mode, and would
    // satisfy a count.
    const QString rules = QString::number(BackendDriver::kStagedRuleCount);
    QVERIFY2(driver.waitUntil([&] {
                 return observer.countAtOrAfter(QStringLiteral("rules"), after, rules) >= 1 &&
                        observer.countAtOrAfter(QStringLiteral("config"), after,
                                                BackendDriver::kStagedMode()) >= 1;
             }),
             qPrintable(QStringLiteral("a generation bump that was not an endpoint change did "
                                       "not re-issue the snapshot set: the rules list and the "
                                       "base configuration were left stale. bumped %1 -> %2, "
                                       "delivered %3")
                            .arg(cb::number(before))
                            .arg(cb::number(after))
                            .arg(observer.digest())));

    // The other half: an endpoint change publishes the live view as EMPTY
    // rather than leaving the previous controller's data on screen.
    const int clearsBefore = observer.liveStateClears;
    backend.attach(driver.controller(1));
    QVERIFY2(driver.waitUntil([&] { return observer.liveStateClears > clearsBefore; }),
             "an endpoint change did not invalidate the live view");
    QVERIFY(backend.removeObserver(&observer));
}

// ------------------------------------------------------------- ownership

void detachLeavesTheAttachedControllerRunning(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));

    QVERIFY(attachAndConnect(driver, 0));
    QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
    QVERIFY(backend.isExternalControllerConnected());
    QCOMPARE(backend.ownership(), cb::Ownership::None);  // nothing managed here

    backend.detach();
    QVERIFY(driver.deliver());
    QVERIFY(!backend.isAttached());
    QVERIFY(!backend.isConnected());
    QCOMPARE(backend.attachmentOwnership(), cb::Ownership::None);
    QVERIFY(!backend.isExternalControllerConnected());

    // Detaching is not a kill switch (section 1). Re-attaching PROVES the
    // controller is still running rather than assuming it.
    QVERIFY2(attachAndConnect(driver, 0),
             "the controller stopped answering after a detach: detach terminated a "
             "controller this component never started");
    QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
    QVERIFY(backend.removeObserver(&observer));
}

void stopTerminatesOnlyTheManagedCore(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    QVERIFY2(driver.has(Ability::ManagedCore), "every driver must be able to run a managed core");

    const QString external = distinctiveText(QStringLiteral("external"));
    QVERIFY(driver.stageVersion(1, external));
    QString why;
    QVERIFY2(bringUpManagedCore(driver, QStringLiteral("a"), &why) != cb::RequestId::Invalid,
             qPrintable(why));
    QCOMPARE(backend.state(), cb::CoreState::Running);
    QCOMPARE(backend.ownership(), cb::Ownership::Managed);
    QVERIFY(cb::isValid(backend.managedEndpoint()));
    QVERIFY(backend.isManagedCoreActive());
    QCOMPARE(backend.activeConfigPaths(), QStringList{driver.configPath(QStringLiteral("a"))});

    // Both alive at the same moment, and independently observable - which is
    // what section 1 says the contract must answer directly instead of making
    // a consumer compare endpoints by hand.
    QVERIFY(attachAndConnect(driver, 1));
    QCOMPARE(backend.ownership(), cb::Ownership::Managed);
    QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
    QVERIFY(backend.isExternalControllerConnected());

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));
    const cb::RequestId stop = backend.stop();
    QVERIFY(stop != cb::RequestId::Invalid);
    QVERIFY2(driver.settleStop(), "the managed core never settled after stop()");
    QVERIFY(driver.deliver());

    QCOMPARE(backend.state(), cb::CoreState::Stopped);
    QVERIFY2(!cb::isValid(backend.managedEndpoint()), "stop() left the managed endpoint behind");
    QCOMPARE(backend.ownership(), cb::Ownership::None);
    QVERIFY(!backend.isManagedCoreActive());
    QVERIFY(!backend.isRestartPending());

    QCOMPARE(static_cast<int>(observer.stops.size()), 1);
    const cb::StopCompleted &result = observer.stops.front();
    QCOMPARE(result.request, stop);
    QVERIFY2(result.confirmed, "the managed child's exit was not observed");
    QCOMPARE(result.status, cb::CompletionStatus::Ok);
    QVERIFY(!result.reason.isFailure());
    QCOMPARE(observer.stoppedCount, 1);
    // A1, restated by B1: a stop is the terminal outcome of an operation that
    // bumps the generation itself, so it carries the POST-bump value. A
    // submit-time stamp is dropped by the mandatory section 2 rejection rule,
    // and with it the shutdown warning that blocks quit.
    QVERIFY2(!cb::isSuperseded(result.generation, observer.lastObserved()),
             "a consumer applying the section 2 rejection rule would have DROPPED this "
             "stop completion (backend-r3 B1)");

    // And the external controller was not touched by any of it: still
    // attached, still connected, still answering.
    QVERIFY2(backend.isAttached(), "stop() detached a controller this component never started");
    QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
    const cb::RequestId probe = backend.refreshVersion();
    QVERIFY(probe != cb::RequestId::Invalid);
    QVERIFY2(driver.waitUntil([&] {
                 const Observer::Record *record = observer.find(QStringLiteral("version"), probe);
                 return record != nullptr && record->completion.isOk();
             }),
             "the external controller stopped answering after a stop that was supposed to "
             "reach only the managed core");
    if (driver.has(Ability::DistinctControllerPayloads)) {
        QCOMPARE(observer.find(QStringLiteral("version"), probe)->payload, external);
    }
    QVERIFY(backend.removeObserver(&observer));
}

// ------------------------------------------------------------ validation

void aFailedValidationLeavesTheRunningConfigurationIntact(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    QVERIFY(driver.has(Ability::ValidationOutcome));

    QString why;
    QVERIFY2(bringUpManagedCore(driver, QStringLiteral("a"), &why) != cb::RequestId::Invalid,
             qPrintable(why));
    const cb::Endpoint running = backend.managedEndpoint();
    const QString runningConfig = driver.configPath(QStringLiteral("a"));
    QCOMPARE(backend.activeConfigPaths(), QStringList{runningConfig});

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));

    QVERIFY(driver.prepareValidation(false));
    const QString candidateConfig = driver.configPath(QStringLiteral("b"));
    const cb::RequestId candidate = driver.startManagedCore(QStringLiteral("b"));
    QVERIFY(candidate != cb::RequestId::Invalid);

    // Validation precedes mutation (section 3): the running core is still live
    // while its replacement is checked in a separate child, and BOTH
    // configurations must stay undeletable while that is true.
    QCOMPARE(backend.state(), cb::CoreState::Running);
    QVERIFY(backend.activeConfigPaths().contains(runningConfig));
    QVERIFY2(backend.activeConfigPaths().contains(candidateConfig),
             "a candidate being validated could be deleted from under its own validator");

    QVERIFY(driver.settleValidation());
    QVERIFY(driver.deliver());

    QCOMPARE(backend.state(), cb::CoreState::Running);
    QCOMPARE(backend.activeConfigPaths(), QStringList{runningConfig});
    QVERIFY2(cb::isSameEndpoint(backend.managedEndpoint(), running),
             "a failed validation moved the running core's endpoint");
    QVERIFY2(observer.states.empty(),
             "a failed validation changed the managed core's state; the running "
             "configuration was supposed to be left intact");
    QVERIFY2(!observer.coreFailures.empty(), "a failed validation reported nothing");
    const cb::Completion &failure = observer.coreFailures.back();
    QCOMPARE(failure.error.code, cb::ErrorCode::ValidationFailed);
    QCOMPARE(failure.status, cb::CompletionStatus::Failed);
    QCOMPARE(failure.request, candidate);
    QVERIFY(!failure.error.message.isEmpty());
    QVERIFY(backend.removeObserver(&observer));
}

// -------------------------------------------------------------- providers

void aDuplicateProviderRequestIsCoalesced(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    QVERIFY(attachAndConnect(driver, 0));

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));
    QVERIFY(driver.hold(Channel::Providers));

    const cb::RequestId first = backend.fetchProviders(true);
    const cb::RequestId again = backend.fetchProviders(true);
    const cb::RequestId other = backend.fetchProviders(false);
    QVERIFY(first != cb::RequestId::Invalid);
    QVERIFY2(again == first,
             "a duplicate submission minted a second RequestId instead of returning the "
             "outstanding one (backend-r2, answer 1)");
    QVERIFY(other != cb::RequestId::Invalid);
    QVERIFY2(other != first, "a different provider operation was coalesced onto this one");
    QVERIFY(backend.isProviderBusy());

    QVERIFY(driver.release(Channel::Providers));
    QVERIFY2(driver.waitUntil([&] { return !backend.isProviderBusy(); }),
             "the provider operations never settled");
    QVERIFY(driver.deliver());

    // One completion per operation: two issued requests would settle twice.
    QCOMPARE(observer.countOf(QStringLiteral("providers/rules"), cb::CompletionStatus::Ok), 1);
    QCOMPARE(observer.countOf(QStringLiteral("providers/proxies"), cb::CompletionStatus::Ok), 1);
    QVERIFY(observer.find(QStringLiteral("providers/rules"), first) != nullptr);
    QVERIFY(observer.find(QStringLiteral("providers/proxies"), other) != nullptr);
    QVERIFY(backend.removeObserver(&observer));
}

// ----------------------------------------------------- the session boundary

namespace {

/// Asserts that `request` was retired: MARKED Superseded (A2), carrying no
/// payload, and turned away by a conforming consumer. `what` names the
/// boundary, so a failure says which one did not retire it.
///
/// `emptyPayload` is what "no payload" LOOKS like on that channel, because the
/// observer records a span as its size: an emptied provider list is "0" and an
/// emptied version is the empty string. The version channel is where the
/// payload discriminates - it carries distinctive staged text - and the
/// provider channel's evidence is the mark and the admission, since both
/// worlds answer a provider fetch with an empty set.
void assertRetired(const Observer &observer, const QString &channel, cb::RequestId request,
                   const QString &emptyPayload, const char *what) {
    const Observer::Record *record = observer.find(channel, request);
    QVERIFY2(record != nullptr,
             qPrintable(QStringLiteral("%1: nothing on %2 ever completed request %3. %4")
                            .arg(QString::fromLatin1(what), channel)
                            .arg(cb::number(request))
                            .arg(observer.digest())));
    QVERIFY2(record->completion.status == cb::CompletionStatus::Superseded,
             qPrintable(QStringLiteral("%1: work owed by the retired session settled as live "
                                       "data on %2 (status %3)")
                            .arg(QString::fromLatin1(what), channel)
                            .arg(static_cast<int>(record->completion.status))));
    QVERIFY2(record->payload == emptyPayload,
             qPrintable(QStringLiteral("%1: a retired completion carried a payload (%2): the "
                                       "previous session's data was published as the new "
                                       "session's")
                            .arg(QString::fromLatin1(what), record->payload)));
    QVERIFY2(!record->admitted,
             qPrintable(QStringLiteral("%1: a conforming consumer would have acted on retired "
                                       "work")
                            .arg(QString::fromLatin1(what))));
}

/// Waits for `request` to complete on `channel` at all, retired or not.
bool settled(BackendDriver &driver, const Observer &observer, const QString &channel,
             cb::RequestId request) {
    return driver.waitUntil(
        [&] { return observer.find(channel, request) != nullptr; });
}

const QString kProvidersRules = QStringLiteral("providers/rules");
const QString kVersion = QStringLiteral("version");

}  // namespace

void aReplacementAtTheSameAddressOpensANewSession(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    QVERIFY(driver.stageVersion(0, distinctiveText(QStringLiteral("retired"))));
    QVERIFY(attachAndConnect(driver, 0));

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));
    QVERIFY(driver.hold(Channel::Providers));
    QVERIFY(driver.hold(Channel::Version));

    // What the engine that is about to be replaced still owes.
    const cb::RequestId retiredProviders = backend.fetchProviders(true);
    const cb::RequestId retiredVersion = backend.refreshVersion();
    QVERIFY(retiredProviders != cb::RequestId::Invalid);
    QVERIFY(retiredVersion != cb::RequestId::Invalid);

    const cb::Generation before = backend.generation();
    const std::size_t addresses = observer.endpoints.size();
    const int clears = observer.liveStateClears;
    const std::size_t transitions = observer.connects.size();

    // The engine is replaced at the host, port and secret it already held.
    backend.attach(driver.controller(0));
    QVERIFY2(backend.generation() > before,
             "re-attaching to a replaced engine left the generation where it was: physical "
             "address equality was treated as session identity, so nothing the retired "
             "process owed was invalidated");

    // A submission made NOW belongs to the new session. It may not join work
    // the retired process owed, and it must really be ISSUED - a coalesced
    // no-op would hand back an id that never settles again.
    const cb::RequestId freshProviders = backend.fetchProviders(true);
    QVERIFY2(freshProviders != cb::RequestId::Invalid,
             "the new session could not submit a provider fetch at all");
    QVERIFY2(freshProviders != retiredProviders,
             "a fetch submitted after the replacement was coalesced onto the request the "
             "RETIRED process owed: it issued nothing, and the caller was handed the id of "
             "work that is already dead");
    // A duplicate of the CURRENT generation still coalesces: the boundary
    // narrows the coalescing window, it does not remove it.
    QCOMPARE(backend.fetchProviders(true), freshProviders);

    QVERIFY(driver.release(Channel::Providers));
    QVERIFY(driver.release(Channel::Version));
    QVERIFY2(settled(driver, observer, kProvidersRules, retiredProviders),
             qPrintable(QStringLiteral("the retired provider request never settled. %1")
                            .arg(driver.describe())));
    QVERIFY2(settled(driver, observer, kVersion, retiredVersion),
             qPrintable(QStringLiteral("the retired version request never settled. %1")
                            .arg(driver.describe())));
    QVERIFY2(settled(driver, observer, kProvidersRules, freshProviders),
             qPrintable(QStringLiteral("the fetch submitted after the replacement never "
                                       "settled, so nothing was issued for it. %1")
                            .arg(driver.describe())));

    assertRetired(observer, kProvidersRules, retiredProviders, QStringLiteral("0"),
                  "same-address replacement");
    assertRetired(observer, kVersion, retiredVersion, QString(), "same-address replacement");
    const Observer::Record *fresh = observer.find(kProvidersRules, freshProviders);
    QVERIFY(fresh != nullptr);
    QVERIFY2(fresh->completion.status == cb::CompletionStatus::Ok,
             "the new session's own fetch did not settle as live work");
    QVERIFY2(fresh->admitted, "a conforming consumer would have rejected the new session's "
                              "own completion");

    // The address did not change, and the replacement is reachable at it.
    QVERIFY2(observer.endpoints.size() == addresses,
             "an endpoint that did not change was published as an endpoint change");
    QVERIFY2(observer.connects.size() == transitions,
             "a replacement at the same address reported the live session as disconnected");
    QVERIFY2(observer.liveStateClears == clears,
             "a replacement at the same address cleared the live view it had just inherited");
    QVERIFY(backend.isConnected());
    QVERIFY(backend.removeObserver(&observer));
}

void aLifecycleBumpRetiresOutstandingWork(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    QVERIFY(driver.stageVersion(0, distinctiveText(QStringLiteral("before-the-bump"))));
    QVERIFY(attachAndConnect(driver, 0));

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));
    QVERIFY(driver.hold(Channel::Providers));
    QVERIFY(driver.hold(Channel::Version));

    const cb::RequestId retiredProviders = backend.fetchProviders(true);
    const cb::RequestId retiredVersion = backend.refreshVersion();
    QVERIFY(retiredProviders != cb::RequestId::Invalid);
    QVERIFY(retiredVersion != cb::RequestId::Invalid);
    const cb::Generation before = backend.generation();

    // A managed stop is a generation bump that no client event announces. The
    // collaborators behind a real backend have no epoch of their own that
    // moves here, which is exactly why this was the hole: their completions
    // arrived Ok, carrying the previous session's payload, under a generation
    // the backend had already left.
    backend.stop();
    QVERIFY2(backend.generation() > before, "a managed stop did not bump the generation");

    const cb::RequestId freshProviders = backend.fetchProviders(true);
    QVERIFY2(freshProviders != retiredProviders,
             "a fetch submitted after a lifecycle bump was coalesced onto work of the "
             "generation that bump retired");
    QCOMPARE(backend.fetchProviders(true), freshProviders);

    QVERIFY(driver.release(Channel::Providers));
    QVERIFY(driver.release(Channel::Version));
    QVERIFY2(settled(driver, observer, kProvidersRules, retiredProviders),
             qPrintable(QStringLiteral("the retired provider request never settled. %1")
                            .arg(driver.describe())));
    QVERIFY2(settled(driver, observer, kVersion, retiredVersion),
             qPrintable(QStringLiteral("the retired version request never settled. %1")
                            .arg(driver.describe())));

    assertRetired(observer, kProvidersRules, retiredProviders, QStringLiteral("0"),
                  "managed stop");
    assertRetired(observer, kVersion, retiredVersion, QString(), "managed stop");
    QVERIFY(backend.removeObserver(&observer));
}

// -------------------------------------------------------------------- TUN

void theTunCompletionIsAReadBackNotAnEcho(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();
    // The controller will report TUN still DISABLED after accepting the
    // change - exactly what happens when the core cannot create the
    // interface. Nothing here creates one.
    QVERIFY(driver.stageTunReadback(false));
    QVERIFY(attachAndConnect(driver, 0));

    Observer observer(&driver);
    QVERIFY(backend.addObserver(&observer));
    QVERIFY(driver.hold(Channel::TunChange));

    const cb::RequestId change = backend.setTunEnabled(true);
    QVERIFY(change != cb::RequestId::Invalid);
    QVERIFY2(backend.isTunChangePending(), "an outstanding TUN change was not reported pending");
    QVERIFY2(backend.setTunEnabled(false) == cb::RequestId::Invalid,
             "a second TUN change was accepted while one was outstanding");

    QVERIFY(driver.release(Channel::TunChange));
    QVERIFY(driver.waitUntil([&] { return !observer.tunChanges.empty(); }));

    const cb::TunChangeCompleted refused = observer.tunChanges.front();
    QCOMPARE(refused.request, change);
    QVERIFY(refused.requested);
    QVERIFY2(!refused.actual,
             "the completion echoed the request instead of reading the controller's state "
             "back: a refused TUN change was reported as a successful one");
    QVERIFY(!backend.isTunChangePending());

    // The other direction, so `actual` is shown to track the controller rather
    // than being a constant: staged enabled, the same call reports enabled.
    QVERIFY(driver.stageTunReadback(true));
    const cb::RequestId again = backend.setTunEnabled(true);
    QVERIFY(again != cb::RequestId::Invalid);
    QVERIFY2(again != change, "a second TUN change reused the first one's RequestId");
    QVERIFY(driver.waitUntil([&] { return observer.tunChanges.size() >= 2; }));
    QVERIFY2(observer.tunChanges.back().actual,
             "the read-back ignored what the controller reported");
    QCOMPARE(observer.tunChanges.back().request, again);
    QVERIFY(backend.removeObserver(&observer));
}

// ------------------------------------------------------ privileged status

void thePrivilegedStatusCarriesTheHelpersRunningCore(BackendDriver &driver) {
    cb::MihomoBackend &backend = driver.backend();

    // backend-r4 C1. The macOS helper keeps one core for the machine and
    // answers every connection from it, so this flag means "a core is alive
    // under the helper, possibly another session's" - which is the condition
    // the uninstall guard needs. A boundary or a backend that dropped it would
    // arm nothing and say nothing, which is exactly what happened for a whole
    // wave.
    for (const bool running : {false, true}) {
        QVERIFY(driver.publishHelperCoreRunning(running));
        Observer observer(&driver);
        QVERIFY(backend.addObserver(&observer));

        const cb::RequestId status = backend.requestPrivilegedServiceStatus();
        QVERIFY2(status != cb::RequestId::Invalid, "the privileged status query was refused");
        QVERIFY2(driver.waitUntil([&] {
                     return observer.find(QStringLiteral("service-status"), status) != nullptr;
                 }),
                 "the privileged status query was never answered");

        const Observer::Record *record =
            observer.find(QStringLiteral("service-status"), status);
        QVERIFY(record != nullptr);
        QCOMPARE(record->completion.status, cb::CompletionStatus::Ok);
        // "<state>/<coreRunning>/<version>" - state, the C1 flag and the
        // helper's version in one comparison.
        QCOMPARE(record->payload,
                 QStringLiteral("%1/%2/helper-1.2.3")
                     .arg(static_cast<int>(cb::ServiceState::Connected))
                     .arg(running ? 1 : 0));
        QVERIFY(backend.removeObserver(&observer));
    }
}

}  // namespace testsupport::backend::common::cases
