// W02 - Subscription update.
//
// docs/TEST_STRATEGY.md, "Complete workflows":
//
//   Exercise  Import remote subscription (local fixture) -> apply preset and
//             overrides -> start -> refresh -> reject an invalid refresh ->
//             quit -> reopen
//   Outcome   The generated configuration is what the layers say it is, a
//             refresh keeps the user's own edits, a rejected refresh loses
//             nothing and costs nothing, and the state survives the process
//
// WHAT MAKES THIS A JOURNEY RATHER THAN A LONGER UNIT TEST
//
// The subscription is a REAL HTTP resource on loopback, fetched by the real
// QNetworkAccessManager inside the real core::ProfileStore, written to a real
// file by its real worker thread, composed by the real composer and launched in
// a real child process through the engine module. The only substitutions are
// the two the isolation rules demand - the OS proxy command and the privileged
// helper - and neither takes part. No URL leaves 127.0.0.1: testsupport::
// LoopbackServer answers the subscription, and an unscripted request is a 501
// the fixture records as unexpected rather than an invented reply.
//
// THE ENGINE IS LOADED, NOT LINKED. Decision D8: the supervisor lives in a
// separately built shared library, and this journey reaches it through
// clashqt::integration::ModuleLoader and ModuleBackend from
// CLASH_QT_MODULE_PATH, exactly as src/main.cpp does. Nothing here names
// core/mihomo/**.
//
// THE FOUR CLAIMS, AND WHAT DECIDES EACH
//
//   "the generated configuration is what the layers say it is"  the BYTES of
//       the file the child was handed, re-read from disk at every stage. Not
//       ProfileStore's opinion of the composition and not a preview: the
//       precedence claim (config-r1: source -> global presets -> profile
//       presets -> overrides -> controller-owned fields) is asserted against
//       the file the engine was actually launched from, which is the only
//       artefact a user's traffic depends on.
//   "a refresh keeps the user's own edits"  the same assertions again after the
//       subscription body changes underneath. A store that regenerated from the
//       downloaded bytes alone would pass everything before the refresh and
//       fail here, which is the regression this journey exists for.
//   "a rejected refresh loses nothing"  a SHA-256 of the cached profile taken
//       before the attempt, the live core's state and generation afterwards,
//       and the backend's own activeConfigPaths(). Both rejection modes are
//       driven separately - a 503 never reaches the file layer, a malformed
//       body reaches it and must be refused there - because they fail in
//       different places.
//   "the state survives the process"  a SECOND graph over the same data
//       directory, after the first is destroyed. Presets, the cached bytes, the
//       edited URL and the parsed quota are read back out of the new objects.
//
// THE PRESET CONNECTION. main.cpp wires ProfileStore::presetsChanged to
// RuntimeCoordinator::scheduleReload (config-r1), and wf::AssembledApp now
// mirrors it. It is asserted BEHAVIOURALLY here rather than structurally: the
// core is already running when the presets are saved, so the reload the
// connection schedules produces a new configuration, and the new configuration
// is what the assertions read. Deleting the connection makes the preset take
// effect only at the next restart, and this case is what notices.
//
// THE TWO ARMS. CLASH_QT_W02_REAL_CORE selects between them and they never run
// together, because the fake-core cases need CLASH_QT_FAKE_CORE and the
// real-core case needs a pinned, source-built engine and the freedom to bind
// real ports:
//   * unset (test `w02-subscription-update`): the fixture journeys below, with
//     the compiled fake core. The real-core case skips and says so.
//   * =1 (test `w02-real-core`): the pinned engine case only, and it does NOT
//     degrade. An absent binary, an absent or unpinned provenance manifest, a
//     checksum that does not match the executable, or a controller port that is
//     already taken is a FAILURE that names the gate it leaves open - a
//     real-core arm that skips itself is how a real-core claim quietly stops
//     being one.

#include <QtTest>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTcpServer>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "support/fake_core.h"
#include "support/loopback_server.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"
#include "workflows/composition_root_audit.h"
#include "workflows/workflow_support.h"

using testsupport::FakeCore;
using testsupport::LoopbackServer;
using testsupport::ScopedEnvironment;

namespace wf = workflows;
namespace cb = core::backend;

namespace {

// ---------------------------------------------------------------- the arms

bool realCoreArm() { return qEnvironmentVariable("CLASH_QT_W02_REAL_CORE") == QLatin1String("1"); }

constexpr auto kFixtureArmSkip =
    "CLASH_QT_W02_REAL_CORE=1: this run is the pinned-engine arm, which needs real ports and no "
    "fake core. The fixture journeys run in the `w02-subscription-update` test, and the two are "
    "registered separately so neither is silently traded for the other.";

/// The submodule commit the engine MUST be built from
/// (.refactor/PROGRESS_LEDGER.md, "Recorded source provenance"). Written down
/// here on purpose: an arm that read the expected commit out of the same
/// manifest it is checking would accept any engine at all.
constexpr auto kPinnedCoreCommit = "ab405bad5beeeac8b003bb01f60f134f6df54471";

// -------------------------------------------------------- the subscription

constexpr auto kSubscriptionPath = "/w02/office.yaml";
constexpr auto kEditedSubscriptionPath = "/w02/office-moved.yaml";

/// A `subscription-userinfo` line, spaced the way a provider's is rather than
/// the way a parser would like it.
QByteArray quotaHeader(quint64 upload, quint64 download, quint64 total, qint64 expireSecs) {
    return "upload=" + QByteArray::number(upload) + "; download= " + QByteArray::number(download) +
           " ;total=" + QByteArray::number(total) + "; expire=" + QByteArray::number(expireSecs);
}

LoopbackServer::Reply subscriptionReply(const QByteArray &body, const QByteArray &quota,
                                        const QByteArray &filename = "w02-office") {
    return LoopbackServer::Reply::document("text/yaml", body)
        .withHeader("subscription-userinfo", quota)
        .withHeader("content-disposition", "attachment; filename=\"" + filename + "\"");
}

/// Everything the controller is asked for on the way up and while running.
void scriptController(LoopbackServer &server) {
    using Reply = LoopbackServer::Reply;
    server.route("GET", "/version", Reply::json(R"({"version":"workflow-core-1.19.31"})"));
    server.route("GET", "/configs", Reply::json(R"({"mode":"rule","tun":{"enable":false}})"));
    server.route("GET", "/proxies",
                 Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]},"DIRECT":{"type":"Direct","now":""}}})"));
    server.route("GET", "/rules",
                 Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
}

// ------------------------------------------------------------ yaml reading

QByteArray unquote(QByteArray value) {
    value = value.trimmed();
    if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
        value.back() == value.front())
        return value.mid(1, value.size() - 2);
    return value;
}

/// The value of a top-level `key:` in a generated configuration, or a null
/// QByteArray when there is none. Deliberately a text read: the assertion is
/// about the file the engine was handed, so it is parsed the way the engine
/// would see it rather than through the object that wrote it.
QByteArray topLevelScalar(const QByteArray &yaml, const QByteArray &key) {
    for (const QByteArray &line : yaml.split('\n')) {
        if (!line.startsWith(key + ":")) continue;  // column 0 only
        return unquote(line.mid(key.size() + 1));
    }
    return {};
}

/// The value of `key:` inside the indented block that follows `block:`.
QByteArray nestedScalar(const QByteArray &yaml, const QByteArray &block, const QByteArray &key) {
    const QList<QByteArray> lines = yaml.split('\n');
    bool inside = false;
    for (const QByteArray &line : lines) {
        if (line.startsWith(block + ":")) {
            inside = true;
            continue;
        }
        if (!inside) continue;
        if (!line.isEmpty() && !line.startsWith(" ")) break;  // the block ended
        const QByteArray trimmed = line.trimmed();
        if (trimmed.startsWith(key + ":")) return unquote(trimmed.mid(key.size() + 1));
    }
    return {};
}

// ----------------------------------------------------------------- presets

QJsonObject operation(const char *op, const char *path, const QJsonValue &value) {
    return QJsonObject{{QStringLiteral("op"), QLatin1String(op)},
                       {QStringLiteral("path"), QLatin1String(path)},
                       {QStringLiteral("value"), value}};
}

QJsonObject preset(const char *id, const char *name, const QJsonArray &operations) {
    return QJsonObject{{QStringLiteral("id"), QLatin1String(id)},
                       {QStringLiteral("name"), QLatin1String(name)},
                       {QStringLiteral("enabled"), true},
                       {QStringLiteral("operations"), operations}};
}

/// One global preset and one scoped to `uid`, arranged so that the ORDER is
/// observable: both write log-level, and the profile-scoped one has to win.
QJsonObject presetDocumentFor(const QString &uid) {
    const QJsonArray global{
        preset("w02-global", "House style",
               QJsonArray{operation("replace", "/allow-lan", true),
                          operation("replace", "/log-level", QStringLiteral("warning"))})};
    const QJsonArray scoped{
        preset("w02-office", "Office profile",
               QJsonArray{operation("replace", "/log-level", QStringLiteral("debug"))})};
    return QJsonObject{{QStringLiteral("version"), 1},
                       {QStringLiteral("global"), global},
                       {QStringLiteral("profiles"), QJsonObject{{uid, scoped}}}};
}

// ------------------------------------------------------------- provenance

QByteArray sha256Of(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result().toHex() : QByteArray();
}

quint16 reserveFreePort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

// ------------------------------------------------------- the ready ledger

/// Every readiness the managed core announced, WITH the generation it carried.
///
/// wf::JourneyObserver records coreReady's endpoint and drops its Completion,
/// and the completion is the half a replacement needs. coreReady is "the
/// managed core answered GET /version with 200", not "a process started"
/// (observer.h), and it is the terminal outcome of an operation that bumped the
/// generation itself, so it carries the POST-bump value (backend-r2 A1,
/// mihomo_backend.cpp:840 stamps `startGeneration_` at the bump). A readiness
/// stamped STRICTLY NEWER than the generation a refresh replaced can therefore
/// only have come from the incoming engine.
///
/// This is why the case does not reason from a process count. `pgrep` says how
/// many children exist, never which engine they are: for most of a reload the
/// single live child is the OUTGOING one, and an assertion that counts it is
/// green for the wrong reason.
class ReadyLedger final : public cb::BackendObserver {
  public:
    struct Entry {
        cb::Generation generation;
        cb::Endpoint endpoint;
    };

    explicit ReadyLedger(wf::AssembledApp &app) : backend_(*app.backend) {
        backend_.addObserver(this);
    }
    ~ReadyLedger() override { backend_.removeObserver(this); }

    ReadyLedger(const ReadyLedger &) = delete;
    ReadyLedger &operator=(const ReadyLedger &) = delete;

    /// The newest readiness stamped after `generation`, or nothing.
    std::optional<Entry> readyAfter(cb::Generation generation) const {
        for (auto entry = entries_.rbegin(); entry != entries_.rend(); ++entry)
            if (cb::number(entry->generation) > cb::number(generation)) return *entry;
        return std::nullopt;
    }

    int count() const { return static_cast<int>(entries_.size()); }

    QString transcript() const {
        QStringList out;
        for (const Entry &entry : entries_)
            out << QStringLiteral("ready(generation=%1,port=%2)")
                       .arg(cb::number(entry.generation))
                       .arg(entry.endpoint.port);
        return out.isEmpty() ? QStringLiteral("none") : out.join(QStringLiteral(", "));
    }

    void coreReady(const cb::Completion &completion, const cb::Endpoint &endpoint) noexcept override {
        entries_.push_back({completion.generation, endpoint});
    }

  private:
    wf::ModuleBackend &backend_;
    std::vector<Entry> entries_;
};

}  // namespace

class W02SubscriptionUpdateTest : public QObject {
    Q_OBJECT

  private slots:

    void init() {
        environment_ = std::make_unique<ScopedEnvironment>(QStringLiteral("w02"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }

    void cleanup() {
        if (!enginePath_.isEmpty()) {
            const int alive = wf::liveProcessesOf(enginePath_);
            QVERIFY2(alive <= 0,
                     qPrintable(QStringLiteral("%1 process(es) from %2 outlived the test")
                                    .arg(alive)
                                    .arg(enginePath_)));
        }
        const QString escape = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escape.isEmpty(), qPrintable(escape));
        environment_.reset();
        enginePath_.clear();
    }

    // --- the graph this journey assembles is the one the application ships ---
    //
    // Runs in BOTH arms: it needs no engine at all, and it is the net under
    // every composition-root statement no journey can reach - including, until
    // the case below drives it, the presetsChanged -> scheduleReload edge.

    void theHarnessStillMirrorsTheCompositionRoot() {
        const QString drift = wf::audit::compositionRootDrift();
        QVERIFY2(drift.isEmpty(), qPrintable(drift));
    }

    // --- the journey ----------------------------------------------------------

    void aSubscriptionIsComposedThroughPresetsStartedRefreshedAndReopened() {
        if (realCoreArm()) QSKIP(kFixtureArmSkip);

        LoopbackServer subscription;
        QVERIFY2(subscription.listen(QString()), "the subscription fixture did not bind");
        const qint64 expireV1 = 2000000000;
        const qint64 expireV2 = 2100000000;
        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(body(QStringLiteral("w02-v1")),
                                             quotaHeader(1024, 2048, 10737418240ULL, expireV1)));
        const QString url = subscription.httpBase() + QLatin1String(kSubscriptionPath);

        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) QSKIP(qPrintable(portSkipReason(relay)));

        FakeCore engine(engineDirectory());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] w02 core up"))
                    .runsForever()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        QString uid;
        QString presetsOnDisk;
        QByteArray cachedV2;

        {
            auto app = std::make_unique<wf::AssembledApp>(proxyLog, osProxy);
            QVERIFY2(app->moduleLoaded(), qPrintable(app->moduleError));
            QCOMPARE(app->moduleArtifact(), wf::moduleArtifactPath());
            app->bootstrap(engine.binaryPath());
            QVERIFY2(app->startupErrors.isEmpty(),
                     qPrintable(app->startupErrors.join(QLatin1Char('\n'))));

            // ---- 1. import the subscription -------------------------------
            app->profiles->importFromUrl(url);
            QVERIFY2(wf::waitFor([&app] { return !app->profiles->profiles().isEmpty(); }),
                     qPrintable(QStringLiteral("the subscription was never imported. %1 | %2")
                                    .arg(app->storeErrors.join(QLatin1Char(' ')),
                                         subscription.redactedTranscript())));
            QVERIFY2(app->storeErrors.isEmpty(),
                     qPrintable(app->storeErrors.join(QLatin1Char('\n'))));
            QCOMPARE(app->profiles->profiles().size(), 1);
            const core::Profile imported = app->profiles->profiles().first();
            uid = imported.uid;
            QCOMPARE(app->profiles->currentUid(), uid);
            QVERIFY2(imported.remote, "an imported subscription is not a remote profile, so it "
                                      "could never be refreshed");
            QCOMPARE(imported.url, url);
            // The name came out of the response header, not the URL.
            QCOMPARE(imported.name, QStringLiteral("w02-office"));
            // The quota and the expiry came out of `subscription-userinfo`.
            QCOMPARE(imported.subscription.upload, 1024ULL);
            QCOMPARE(imported.subscription.download, 2048ULL);
            QCOMPARE(imported.subscription.total, 10737418240ULL);
            QCOMPARE(imported.subscription.expire, QDateTime::fromSecsSinceEpoch(expireV1));
            // The bytes on disk are the bytes served.
            QCOMPARE(wf::readTextFile(imported.filePath), body(QStringLiteral("w02-v1")));
            QCOMPARE(subscription.requestCount("GET", QLatin1String(kSubscriptionPath)), 1);

            // ---- 2. the user's own overrides ------------------------------
            QVERIFY2(app->profiles->setRuntimeOverrides(overrides()),
                     qPrintable(app->storeErrors.join(QLatin1Char('\n'))));

            // ---- 3. start, through the module -----------------------------
            QStringList launched;
            QObject::connect(app->runtimeCoordinator.get(),
                             &app::runtime::RuntimeCoordinator::coreStartRequested,
                             app->runtimeCoordinator.get(),
                             [&launched](const QString &path) { launched.append(path); });

            QVERIFY(app->runtimeCoordinator->requestAutostart());
            QVERIFY2(wf::waitFor([&app] { return app->backend->state() == cb::CoreState::Running; }),
                     qPrintable(report(*app, controller)));
            QCOMPARE(launched.size(), 1);
            const QByteArray first = wf::readTextFile(launched.last());
            QVERIFY2(!first.isEmpty(), "the core was launched from a configuration that is gone");
            // The override won over the subscription...
            QCOMPARE(topLevelScalar(first, "mode"), QByteArrayLiteral("global"));
            QCOMPARE(topLevelScalar(first, "mixed-port"), QByteArrayLiteral("27891"));
            // ...the controller-owned fields were reasserted...
            QCOMPARE(wf::controllerPortOf(launched.last()), wf::kGeneratedControllerPort);
            QCOMPARE(nestedScalar(first, "tun", "enable"), QByteArrayLiteral("false"));
            // ...the subscription's own content came through...
            QVERIFY2(first.contains("w02-v1"), "the running core was not launched from the "
                                               "subscription that was downloaded");
            // ...and NOTHING preset-shaped has happened yet, because no preset
            // has been saved. This is the control for step 4.
            QCOMPARE(topLevelScalar(first, "log-level"), QByteArrayLiteral("info"));
            QCOMPARE(topLevelScalar(first, "allow-lan"), QByteArrayLiteral("false"));

            // ---- 4. save presets while the core is RUNNING ----------------
            // main.cpp's presetsChanged -> scheduleReload, driven rather than
            // inspected: the reload it schedules is what produces the
            // configuration asserted below.
            QSignalSpy presetsChanged(app->profiles.get(), &core::ProfileStore::presetsChanged);
            QVERIFY2(app->profiles->setPresetDocument(presetDocumentFor(uid)),
                     qPrintable(app->storeErrors.join(QLatin1Char('\n'))));
            QCOMPARE(presetsChanged.size(), 1);
            QVERIFY2(app->runtimeCoordinator->isReloadScheduled(),
                     "saving a preset scheduled no reload: the presetsChanged -> scheduleReload "
                     "connection is missing, so a saved preset would reach the engine only at the "
                     "next profile switch or restart");
            QVERIFY2(wf::waitFor([&launched] { return launched.size() >= 2; }),
                     qPrintable(report(*app, controller)));
            QVERIFY2(wf::waitFor([&app] { return app->backend->state() == cb::CoreState::Running; }),
                     qPrintable(report(*app, controller)));

            const QByteArray composed = wf::readTextFile(launched.last());
            // The precedence rule, on the file the engine was handed:
            //   profile preset beats global preset...
            QCOMPARE(topLevelScalar(composed, "log-level"), QByteArrayLiteral("debug"));
            //   ...the global preset still applies where nothing overrides it...
            QCOMPARE(topLevelScalar(composed, "allow-lan"), QByteArrayLiteral("true"));
            //   ...and the user's overrides still beat both.
            QCOMPARE(topLevelScalar(composed, "mode"), QByteArrayLiteral("global"));
            QCOMPARE(topLevelScalar(composed, "mixed-port"), QByteArrayLiteral("27891"));
            QCOMPARE(wf::controllerPortOf(launched.last()), wf::kGeneratedControllerPort);
            QVERIFY(composed.contains("w02-v1"));
            // The configuration the BACKEND says it is running is the one just
            // asserted, not merely the one the coordinator generated.
            QVERIFY2(app->backend->activeConfigPaths().contains(launched.last()),
                     qPrintable(QStringLiteral("the engine is running %1, not the composed %2")
                                    .arg(app->backend->activeConfigPaths().join(QLatin1Char(' ')),
                                         launched.last())));
            // Retention: the configuration the engine is running is kept, and
            // nothing this journey did not launch is. A superseded snapshot may
            // still be tracked - RuntimeCoordinator prunes on coreReady and
            // keeps whatever the backend still calls active (backend-r2 5.1,
            // the cancelled validation child) - so the assertion is about what
            // is retained and why, not about an exact count. The quit below is
            // where "nothing survives" is asserted.
            const QStringList tracked = app->runtimeCoordinator->trackedSnapshots();
            QVERIFY2(tracked.contains(launched.last()),
                     qPrintable(QStringLiteral("the running configuration %1 is not retained; "
                                               "tracked: %2")
                                    .arg(launched.last(), tracked.join(QLatin1Char(' ')))));
            for (const QString &path : tracked)
                QVERIFY2(launched.contains(path),
                         qPrintable(QStringLiteral("a snapshot nothing in this journey launched "
                                                   "is retained: %1").arg(path)));

            // ---- 5. the subscription changes underneath -------------------
            subscription.route("GET", kSubscriptionPath,
                               subscriptionReply(body(QStringLiteral("w02-v2")),
                                                 quotaHeader(4096, 8192, 21474836480ULL, expireV2)));
            QSignalSpy updated(app->profiles.get(), &core::ProfileStore::profileUpdated);
            app->profiles->updateProfile(uid);
            QVERIFY2(wf::waitFor([&updated] { return !updated.isEmpty(); }),
                     qPrintable(QStringLiteral("the refresh never completed. %1 | %2")
                                    .arg(app->storeErrors.join(QLatin1Char(' ')),
                                         subscription.redactedTranscript())));
            QCOMPARE(updated.first().at(0).toString(), uid);
            QVERIFY2(app->storeErrors.isEmpty(),
                     qPrintable(app->storeErrors.join(QLatin1Char('\n'))));
            QCOMPARE(subscription.requestCount("GET", QLatin1String(kSubscriptionPath)), 2);

            const core::Profile refreshed = app->profiles->profiles().first();
            cachedV2 = body(QStringLiteral("w02-v2"));
            QCOMPARE(wf::readTextFile(refreshed.filePath), cachedV2);
            // The new quota replaced the old one rather than being merged with it.
            QCOMPARE(refreshed.subscription.total, 21474836480ULL);
            QCOMPARE(refreshed.subscription.upload, 4096ULL);
            QCOMPARE(refreshed.subscription.expire, QDateTime::fromSecsSinceEpoch(expireV2));

            // The refresh reaches the engine (profileUpdated -> onProfileUpdated
            // -> scheduleReload) and the user's layers survive it. THIS is the
            // claim the whole case exists for.
            QVERIFY2(wf::waitFor([&launched] { return launched.size() >= 3; }),
                     qPrintable(report(*app, controller)));
            QVERIFY2(wf::waitFor([&app] { return app->backend->state() == cb::CoreState::Running; }),
                     qPrintable(report(*app, controller)));
            const QByteArray afterRefresh = wf::readTextFile(launched.last());
            QVERIFY2(afterRefresh.contains("w02-v2"),
                     "the refreshed subscription never reached the engine");
            QVERIFY2(!afterRefresh.contains("w02-v1"),
                     "the engine is still running the superseded subscription body");
            QCOMPARE(topLevelScalar(afterRefresh, "log-level"), QByteArrayLiteral("debug"));
            QCOMPARE(topLevelScalar(afterRefresh, "allow-lan"), QByteArrayLiteral("true"));
            QCOMPARE(topLevelScalar(afterRefresh, "mode"), QByteArrayLiteral("global"));
            QCOMPARE(topLevelScalar(afterRefresh, "mixed-port"), QByteArrayLiteral("27891"));
            QCOMPARE(wf::controllerPortOf(launched.last()), wf::kGeneratedControllerPort);
            QVERIFY(app->backend->activeConfigPaths().contains(launched.last()));

            // ---- 6. quit --------------------------------------------------
            presetsOnDisk = app->profiles->dataDir() + QStringLiteral("/presets.json");
            QVERIFY2(QFileInfo::exists(presetsOnDisk), "the presets were not persisted");
            QCOMPARE(app->unknownModuleEvents(), std::uint64_t(0));
            QVERIFY2(app->quit(), qPrintable(QStringLiteral("the quit never completed: %1 | %2")
                                                 .arg(app->blockingReason(),
                                                      report(*app, controller))));
            QVERIFY2(app->events.stops.back().confirmed,
                     "the quit did not confirm the owned child's exit");
            QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                     "the owned child outlived the quit");
            QVERIFY(snapshots().isEmpty());
            // A subscription journey must not have touched the machine's proxy.
            QCOMPARE(proxyLog->count(wf::ProxyAction::Enable), 0);
            QCOMPARE(proxyLog->count(wf::ProxyAction::Disable), 0);
        }

        // ---- 7. reopen ----------------------------------------------------
        {
            auto app = std::make_unique<wf::AssembledApp>(proxyLog, osProxy);
            QVERIFY2(app->moduleLoaded(), qPrintable(app->moduleError));
            app->bootstrap(engine.binaryPath());
            QVERIFY2(app->startupErrors.isEmpty(),
                     qPrintable(app->startupErrors.join(QLatin1Char('\n'))));

            QCOMPARE(app->profiles->profiles().size(), 1);
            const core::Profile persisted = app->profiles->profiles().first();
            QCOMPARE(persisted.uid, uid);
            QCOMPARE(app->profiles->currentUid(), uid);
            QCOMPARE(persisted.url, url);
            QVERIFY(persisted.remote);
            // The cached subscription, byte for byte, and the quota that came
            // with it - read out of a store that has only ever seen the disk.
            QCOMPARE(wf::readTextFile(persisted.filePath), cachedV2);
            QCOMPARE(persisted.subscription.total, 21474836480ULL);
            QCOMPARE(persisted.subscription.expire, QDateTime::fromSecsSinceEpoch(expireV2));
            // The presets, normalised the same way, and still scoped to the
            // same profile.
            const QJsonObject reloaded = app->profiles->presetDocument();
            QCOMPARE(reloaded.value(QStringLiteral("version")).toInt(), 1);
            QCOMPARE(reloaded.value(QStringLiteral("global")).toArray().size(), 1);
            QCOMPARE(reloaded.value(QStringLiteral("global")).toArray().at(0).toObject()
                         .value(QStringLiteral("id")).toString(),
                     QStringLiteral("w02-global"));
            const QJsonObject scoped =
                reloaded.value(QStringLiteral("profiles")).toObject();
            QVERIFY2(scoped.contains(uid), qPrintable(QStringLiteral(
                         "the profile-scoped preset lost its profile: %1")
                             .arg(QString::fromUtf8(QJsonDocument(reloaded).toJson()))));
            QCOMPARE(scoped.value(uid).toArray().at(0).toObject()
                         .value(QStringLiteral("id")).toString(),
                     QStringLiteral("w02-office"));
            QVERIFY2(app->profiles->lastPresetDiagnostics().isEmpty(),
                     "a clean preset document was reloaded with diagnostics");
            // And the overrides, which are a separate file.
            QCOMPARE(app->profiles->runtimeOverrides(), overrides());

            QVERIFY2(app->quit(), qPrintable(app->blockingReason()));
        }

        QVERIFY2(!subscription.sawUnexpectedRequest(),
                 qPrintable(subscription.redactedTranscript()));
        QVERIFY2(!controller.sawUnexpectedRequest(), qPrintable(controller.redactedTranscript()));
    }

    // --- a refresh that must not cost anything -------------------------------
    //
    // Two rejections, driven separately because they are refused in different
    // places: a 503 never reaches the file layer at all, and a body that is not
    // a Clash configuration reaches the write worker and has to be refused
    // there. In both cases the cached bytes, the parsed quota and the RUNNING
    // core have to come through untouched - a store that truncated the cache
    // before writing, or a coordinator that reloaded on a failed refresh, would
    // take a working core down for a provider's bad minute.

    void aRejectedRefreshPreservesTheCachedSubscriptionAndTheLiveCore() {
        if (realCoreArm()) QSKIP(kFixtureArmSkip);

        LoopbackServer subscription;
        QVERIFY(subscription.listen(QString()));
        const qint64 expire = 2000000000;
        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(body(QStringLiteral("w02-good")),
                                             quotaHeader(1, 2, 3000, expire)));
        const QString url = subscription.httpBase() + QLatin1String(kSubscriptionPath);

        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) QSKIP(qPrintable(portSkipReason(relay)));

        FakeCore engine(engineDirectory());
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] w02 core up"))
                    .runsForever()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy);
        QVERIFY2(app.moduleLoaded(), qPrintable(app.moduleError));
        app.bootstrap(engine.binaryPath());

        app.profiles->importFromUrl(url);
        QVERIFY2(wf::waitFor([&app] { return !app.profiles->profiles().isEmpty(); }),
                 qPrintable(subscription.redactedTranscript()));
        const core::Profile profile = app.profiles->profiles().first();
        const QString uid = profile.uid;
        const QByteArray cached = wf::readTextFile(profile.filePath);
        QCOMPARE(cached, body(QStringLiteral("w02-good")));

        QStringList launched;
        QObject::connect(app.runtimeCoordinator.get(),
                         &app::runtime::RuntimeCoordinator::coreStartRequested,
                         app.runtimeCoordinator.get(),
                         [&launched](const QString &path) { launched.append(path); });
        QVERIFY(app.runtimeCoordinator->requestAutostart());
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                 qPrintable(report(app, controller)));
        QCOMPARE(launched.size(), 1);
        const QString live = launched.last();
        const cb::Generation runningGeneration = app.backend->generation();

        // ---- a body that is not a Clash configuration ---------------------
        // A YAML mapping with neither `proxies` nor `proxy-providers`: it
        // parses, so this is a refusal by the store's own validation rather
        // than by the YAML parser.
        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(QByteArrayLiteral("note: this is not a profile\n"),
                                             quotaHeader(9, 9, 9, expire + 1)));
        app.storeErrors.clear();
        app.profiles->updateProfile(uid);
        QVERIFY2(wf::waitFor([&app] { return !app.storeErrors.isEmpty(); }),
                 qPrintable(QStringLiteral("a malformed subscription was accepted in silence. %1")
                                .arg(subscription.redactedTranscript())));
        QVERIFY2(app.storeErrors.join(QLatin1Char(' ')).contains(profile.name),
                 qPrintable(QStringLiteral("the failure does not name the profile it concerns: %1")
                                .arg(app.storeErrors.join(QLatin1Char(' ')))));
        // The cache is what it was, to the byte.
        QCOMPARE(wf::readTextFile(profile.filePath), cached);
        // And the quota was not replaced by the rejected response's.
        QCOMPARE(app.profiles->profiles().first().subscription.total, 3000ULL);
        QCOMPARE(app.profiles->profiles().first().subscription.expire,
                 QDateTime::fromSecsSinceEpoch(expire));

        // ---- an HTTP failure ----------------------------------------------
        subscription.route("GET", kSubscriptionPath, LoopbackServer::Reply::failure(503));
        app.storeErrors.clear();
        app.profiles->updateProfile(uid);
        QVERIFY2(wf::waitFor([&app] { return !app.storeErrors.isEmpty(); }),
                 qPrintable(QStringLiteral("a 503 refresh was accepted in silence. %1")
                                .arg(subscription.redactedTranscript())));
        QCOMPARE(wf::readTextFile(profile.filePath), cached);
        QCOMPARE(app.profiles->profiles().first().subscription.total, 3000ULL);

        // ---- the live core is untouched by either ------------------------
        // Given time to go wrong: the reload debounce is 100 ms, so a
        // coordinator that reloaded on a failed refresh would have done it by
        // now, and the absence of an event cannot be awaited on a predicate.
        QVERIFY(wf::drainPast(400));
        QVERIFY(app.backend->drain());
        QCOMPARE(app.backend->state(), cb::CoreState::Running);
        QCOMPARE(launched.size(), 1);
        QCOMPARE(launched.last(), live);
        QCOMPARE(cb::number(app.backend->generation()), cb::number(runningGeneration));
        QVERIFY2(app.backend->activeConfigPaths().contains(live),
                 "the engine stopped running the configuration a rejected refresh never replaced");
        QCOMPARE(wf::liveProcessesOf(engine.binaryPath()), 1);
        QVERIFY2(app.events.coreFailures.empty(),
                 "a rejected subscription refresh was reported as a core failure");

        QVERIFY2(app.quit(), qPrintable(app.blockingReason()));
        QVERIFY(wf::awaitNoLiveProcess(engine.binaryPath()));
        QVERIFY(snapshots().isEmpty());
    }

    // --- the latest URL wins, and a stale body never lands -------------------
    //
    // The user edits the subscription URL while a refresh of the OLD one is
    // still in flight. ProfileStore aborts the superseded reply and keys the
    // late handler on reply identity; this case releases the held response
    // AFTERWARDS, which is the only way to see whether the abort is real. No
    // core is started: nothing here concerns the engine, and taking the fixed
    // controller port would only make the case fragile.

    void aHeldRefreshLosesToAUrlEditAndTheLatestUrlWins() {
        if (realCoreArm()) QSKIP(kFixtureArmSkip);

        LoopbackServer subscription;
        QVERIFY(subscription.listen(QString()));
        const QByteArray original = body(QStringLiteral("w02-original"));
        const QByteArray stale = body(QStringLiteral("w02-stale"));
        const QByteArray moved = body(QStringLiteral("w02-moved"));
        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(original, quotaHeader(1, 1, 100, 2000000000)));
        subscription.route("GET", kEditedSubscriptionPath,
                           subscriptionReply(moved, quotaHeader(5, 5, 500, 2100000000)));
        const QString url = subscription.httpBase() + QLatin1String(kSubscriptionPath);
        const QString editedUrl = subscription.httpBase() + QLatin1String(kEditedSubscriptionPath);

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy);
        QVERIFY2(app.moduleLoaded(), qPrintable(app.moduleError));
        app.bootstrap(QString());

        app.profiles->importFromUrl(url);
        QVERIFY2(wf::waitFor([&app] { return !app.profiles->profiles().isEmpty(); }),
                 qPrintable(subscription.redactedTranscript()));
        const core::Profile profile = app.profiles->profiles().first();
        const QString uid = profile.uid;
        QCOMPARE(wf::readTextFile(profile.filePath), original);

        // The refresh is parked, and the body it would deliver is the stale one.
        auto *held = subscription.hold("GET", QLatin1String(kSubscriptionPath));
        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(stale, quotaHeader(7, 7, 700, 2050000000)));
        QSignalSpy updated(app.profiles.get(), &core::ProfileStore::profileUpdated);
        app.profiles->updateProfile(uid);
        QVERIFY2(held->waitForPending(1), qPrintable(subscription.pendingReport()));

        // The user moves the subscription while that request is in flight.
        QVERIFY2(app.profiles->setSubscriptionUrl(uid, editedUrl),
                 qPrintable(app.storeErrors.join(QLatin1Char('\n'))));
        QCOMPARE(app.profiles->profiles().first().url, editedUrl);

        // Now the old server answers. The body belongs to a URL this profile no
        // longer has, and it must not become the cache.
        held->release();
        QVERIFY(wf::drainPast(400));
        QCOMPARE(wf::readTextFile(profile.filePath), original);
        QVERIFY2(updated.isEmpty(),
                 "a response from the superseded subscription was accepted as an update");
        QCOMPARE(app.profiles->profiles().first().subscription.total, 100ULL);

        // The next refresh uses the URL the user chose, and that one lands.
        app.profiles->updateProfile(uid);
        QVERIFY2(wf::waitFor([&updated] { return !updated.isEmpty(); }),
                 qPrintable(QStringLiteral("the edited URL was never fetched. %1")
                                .arg(subscription.redactedTranscript())));
        QCOMPARE(subscription.requestCount("GET", QLatin1String(kEditedSubscriptionPath)), 1);
        QCOMPARE(wf::readTextFile(profile.filePath), moved);
        QCOMPARE(app.profiles->profiles().first().subscription.total, 500ULL);
        QVERIFY2(app.storeErrors.isEmpty(), qPrintable(app.storeErrors.join(QLatin1Char('\n'))));
        QVERIFY2(!subscription.sawUnexpectedRequest(),
                 qPrintable(subscription.redactedTranscript()));

        QVERIFY2(app.quit(), qPrintable(app.blockingReason()));
    }

    // --- the pinned, source-built engine -------------------------------------
    //
    // Everything above is proven against the compiled fake core, which is what
    // makes those journeys deterministic. This case is the other half: the SAME
    // subscription journey against the engine this repository builds from
    // 3rdparty/mihomo, loaded through the SAME module, to show that the
    // composition the fake core accepted is a configuration the real engine
    // accepts and that a refresh it rejects does not cost a running core.
    //
    // It asserts its own subject first. A "real core" arm that ran against
    // whatever binary happened to be on the machine would be worth less than no
    // arm at all, so the provenance manifest is checked against the pinned
    // commit, the checksum is recomputed from the executable, and the
    // executable is asked for its own version.

    void theRealSourceBuiltCoreRefreshesAndRejectsAnInvalidUpdate() {
        if (!realCoreArm()) {
            QSKIP("Opt-in: the pinned-engine arm runs as the `w02-real-core` test, which sets "
                  "CLASH_QT_W02_REAL_CORE=1, CLASH_QT_CORE_BINARY and CLASH_QT_CORE_PROVENANCE. "
                  "It is a separate registration rather than a conditional inside this run "
                  "because it needs a built engine and real ports.");
        }

        // ---- 1. the engine is the one this repository builds ---------------
        const QString binary = qEnvironmentVariable("CLASH_QT_CORE_BINARY");
        const QString manifestPath = qEnvironmentVariable("CLASH_QT_CORE_PROVENANCE");
        QVERIFY2(!binary.isEmpty(),
                 "CLASH_QT_W02_REAL_CORE=1 but CLASH_QT_CORE_BINARY is unset. This is a FAILURE "
                 "and not a skip: the arm was asked for, and an arm that quietly does nothing is "
                 "how a real-core claim stops being one. Build the engine with `make core`.");
        QVERIFY2(QFileInfo(binary).isExecutable(),
                 qPrintable(QStringLiteral("CLASH_QT_CORE_BINARY names %1, which is not an "
                                           "executable file").arg(binary)));
        QVERIFY2(!manifestPath.isEmpty(),
                 "CLASH_QT_W02_REAL_CORE=1 but CLASH_QT_CORE_PROVENANCE is unset, so the engine's "
                 "origin cannot be established and running it would prove nothing about the "
                 "pinned source.");
        QFile manifestFile(manifestPath);
        QVERIFY2(manifestFile.open(QIODevice::ReadOnly),
                 qPrintable(QStringLiteral("cannot read %1: %2")
                                .arg(manifestPath, manifestFile.errorString())));
        QJsonParseError parse{};
        const QJsonDocument document = QJsonDocument::fromJson(manifestFile.readAll(), &parse);
        QVERIFY2(document.isObject(), qPrintable(QStringLiteral("%1 is not a JSON object: %2")
                                                     .arg(manifestPath, parse.errorString())));
        const QJsonObject manifest = document.object();

        QCOMPARE(manifest.value(QStringLiteral("source_commit")).toString(),
                 QLatin1String(kPinnedCoreCommit));
        const QString describe = manifest.value(QStringLiteral("source_describe")).toString();
        QVERIFY2(!describe.isEmpty(), "the provenance manifest records no source version");
        // The checksum and the size are recomputed from the file on disk: a
        // manifest beside a rebuilt-by-hand binary is exactly the case this
        // catches.
        QCOMPARE(sha256Of(binary),
                 manifest.value(QStringLiteral("artifact_sha256")).toString().toLatin1());
        QCOMPARE(QFileInfo(binary).size(),
                 static_cast<qint64>(manifest.value(QStringLiteral("artifact_bytes")).toDouble()));
        QCOMPARE(manifest.value(QStringLiteral("source_state")).toString(),
                 QStringLiteral("clean"));

        // ...and the executable agrees about what it is.
        QProcess version;
        version.setProcessChannelMode(QProcess::MergedChannels);
        version.start(binary, {QStringLiteral("-v")});
        QVERIFY2(version.waitForStarted(10000), qPrintable(version.errorString()));
        QVERIFY2(version.waitForFinished(15000), qPrintable(version.errorString()));
        const QString reported = QString::fromUtf8(version.readAll());
        QVERIFY2(reported.contains(describe),
                 qPrintable(QStringLiteral("the engine reports \"%1\", which does not carry the "
                                           "manifest's version %2")
                                .arg(reported.trimmed(), describe)));
        // The toolchain the manifest records is the one stamped into the binary.
        const QString toolchain = manifest.value(QStringLiteral("go_toolchain")).toString();
        const QString goVersion = toolchain.section(QLatin1Char(' '), 2, 2);
        QVERIFY2(!goVersion.isEmpty(), qPrintable(QStringLiteral(
                     "the manifest's go_toolchain \"%1\" has no version token").arg(toolchain)));
        QVERIFY2(reported.contains(goVersion),
                 qPrintable(QStringLiteral("the engine reports \"%1\", not the manifest's %2")
                                .arg(reported.trimmed(), goVersion)));

        // ---- 2. the ports this arm needs ---------------------------------
        // The generated controller address is fixed at 29097 by ProfileStore,
        // and the real engine binds it itself - there is no relay to put in
        // front of it. An occupied port is reported as the open gate it is,
        // rather than skipped: the arm was explicitly requested.
        {
            QTcpServer probe;
            QVERIFY2(probe.listen(QHostAddress::LocalHost, wf::kGeneratedControllerPort),
                     qPrintable(QStringLiteral(
                                    "GATE LEFT OPEN: 127.0.0.1:%1 is already in use (%2), and "
                                    "core::ProfileStore writes that address into every "
                                    "configuration it generates, so the pinned engine cannot be "
                                    "reached on it. A running clash-qt core is the usual reason. "
                                    "This arm does not assert something weaker in its place: stop "
                                    "the other core and re-run w02-real-core.")
                                    .arg(wf::kGeneratedControllerPort)
                                    .arg(probe.errorString())));
            probe.close();
        }
        const quint16 mixedPort = reserveFreePort();
        QVERIFY2(mixedPort != 0, "no free loopback port for the engine's mixed inbound");

        LoopbackServer subscription;
        QVERIFY(subscription.listen(QString()));
        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(body(QStringLiteral("w02-real-v1")),
                                             quotaHeader(10, 20, 30000, 2000000000)));
        const QString url = subscription.httpBase() + QLatin1String(kSubscriptionPath);

        // ---- 3. the journey ------------------------------------------------
        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();
        wf::AssembledApp app(proxyLog, osProxy);
        QVERIFY2(app.moduleLoaded(), qPrintable(app.moduleError));
        // Registered before anything is started, so the FIRST engine's readiness
        // is on record too: the refresh below is told apart from it by the
        // generation each one carries, not by arrival order.
        ReadyLedger ready(app);
        app.bootstrap(binary);
        QVERIFY2(app.startupErrors.isEmpty(),
                 qPrintable(app.startupErrors.join(QLatin1Char('\n'))));
        // TUN stays off and the inbound is a port this process just proved
        // free. Nothing here asks for the privileged helper, the system proxy
        // or a VPN interface, and the engine is given no remote node to reach.
        QJsonObject realOverrides{{QStringLiteral("mixed-port"), mixedPort},
                                  {QStringLiteral("allow-lan"), false}};
        QVERIFY2(app.profiles->setRuntimeOverrides(realOverrides),
                 qPrintable(app.storeErrors.join(QLatin1Char('\n'))));

        // KEEPING THE ENGINE OFF THE NETWORK.
        // `external-ui` and `external-ui-url` are controller-owned fields
        // (config-r1): ProfileStore writes <dataDir>/ui and the dashboard URL
        // into every configuration and no preset or override may change them.
        // The pinned engine downloads the dashboard on startup when that
        // directory is EMPTY, and skips it when it is not
        // (3rdparty/mihomo/component/updater/update_ui.go, AutoDownloadUI). A
        // real download is an outbound request the isolation rules forbid, so
        // the directory is seeded here - and the engine's own log line is
        // asserted below, which turns "this arm stayed on loopback" into
        // evidence rather than an intention.
        const QString uiDir = app.profiles->dataDir() + QStringLiteral("/ui");
        QVERIFY(wf::writeTextFile(uiDir + QStringLiteral("/index.html"),
                                  QByteArrayLiteral("<!-- w02 fixture: the engine must not "
                                                    "download a dashboard -->\n")));

        app.profiles->importFromUrl(url);
        QVERIFY2(wf::waitFor([&app] { return !app.profiles->profiles().isEmpty(); }),
                 qPrintable(subscription.redactedTranscript()));
        const core::Profile profile = app.profiles->profiles().first();
        const QString uid = profile.uid;

        QStringList launched;
        QObject::connect(app.runtimeCoordinator.get(),
                         &app::runtime::RuntimeCoordinator::coreStartRequested,
                         app.runtimeCoordinator.get(),
                         [&launched](const QString &path) { launched.append(path); });

        // The real engine validates the configuration in its own child process
        // and then runs it; readiness is its own controller answering GET
        // /version. The budget is the module's published one - ten seconds of
        // silence - so this wait is sized from it rather than from a guess.
        const int readiness = wf::deadlineFor(app.backend->timings().idleDeadlineMs);
        QVERIFY(app.runtimeCoordinator->requestAutostart());
        QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; },
                             readiness),
                 qPrintable(QStringLiteral("the pinned engine never became ready. %1 | logs: %2")
                                .arg(report(app, subscription),
                                     coreLog(app).join(QLatin1Char('\n')))));
        QCOMPARE(launched.size(), 1);
        const QString live = launched.last();
        const QByteArray generated = wf::readTextFile(live);
        QCOMPARE(topLevelScalar(generated, "mixed-port"), QByteArray::number(mixedPort));
        QCOMPARE(nestedScalar(generated, "tun", "enable"), QByteArrayLiteral("false"));
        QCOMPARE(wf::controllerPortOf(live), wf::kGeneratedControllerPort);
        QVERIFY2(wf::waitFor([&app] { return app.backend->isConnected(); }, readiness),
                 "the engine came up but its controller never answered this process");
        QVERIFY(app.backend->drain());
        QCOMPARE(static_cast<int>(app.events.readyEndpoints.size()), 1);
        QCOMPARE(app.events.readyEndpoints.front().port, wf::kGeneratedControllerPort);
        QCOMPARE(wf::liveProcessesOf(binary), 1);
        // The engine says, in its own log, that it did not fetch a dashboard.
        const QString startupLog = coreLog(app).join(QLatin1Char('\n'));
        QVERIFY2(!startupLog.contains(QStringLiteral("External UI downloading")),
                 qPrintable(QStringLiteral("the pinned engine tried to download its dashboard, "
                                           "which is an outbound request this arm must not make. "
                                           "The seeded external-ui directory did not take: %1")
                                .arg(startupLog)));
        QVERIFY2(startupLog.contains(QStringLiteral("UI already exists")),
                 qPrintable(QStringLiteral("the engine did not report skipping the dashboard "
                                           "download, so the guard above is no longer observable "
                                           "and this arm can no longer show that it stayed on "
                                           "loopback: %1").arg(startupLog)));
        QVERIFY2(startupLog.contains(QStringLiteral("RESTful API listening at: 127.0.0.1:%1")
                                         .arg(wf::kGeneratedControllerPort)),
                 qPrintable(QStringLiteral("the engine did not bind the generated controller "
                                           "address: %1").arg(startupLog)));

        // ---- 4. a successful refresh --------------------------------------
        // The starting generation is read once the FIRST engine has stopped
        // moving, so that the advance the refresh has to make below is the
        // refresh's and not the initial attach handoff's.
        cb::Generation generationBefore = cb::Generation::Initial;
        QVERIFY2(awaitSettledGeneration(app, 0, readiness, &generationBefore),
                 qPrintable(QStringLiteral("the pinned engine never settled after its start. %1")
                                .arg(report(app, subscription))));

        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(body(QStringLiteral("w02-real-v2")),
                                             quotaHeader(11, 21, 30000, 2100000000)));
        QSignalSpy updated(app.profiles.get(), &core::ProfileStore::profileUpdated);
        app.profiles->updateProfile(uid);
        QVERIFY2(wf::waitFor([&updated] { return !updated.isEmpty(); }),
                 qPrintable(subscription.redactedTranscript()));
        QVERIFY2(wf::waitFor([&launched] { return launched.size() >= 2; }, readiness),
                 qPrintable(report(app, subscription)));

        // THE HANDOFF IS WAITED FOR BEFORE ANYTHING IS COUNTED.
        //
        // "Running" is not "the refresh landed", and neither is "a second start
        // was requested". RuntimeCoordinator calls backend.start() and only
        // then emits coreStartRequested, and the host answers that start by
        // validating the candidate in a SEPARATE child while the outgoing core
        // keeps running (lifecycle.h:76-85). Measured at the instant the second
        // start is seen, every run of this arm reports: state Running,
        // isConnected() true, generation UNCHANGED, one readiness on record,
        // isRestartPending() true and TWO paths in activeConfigPaths() - the
        // outgoing core's and the candidate being validated. Every one of those
        // describes the engine that is going away.
        //
        // So an assertion placed there is reading the OUTGOING generation. A
        // count of one is the outgoing child, not the refreshed one; a count of
        // two is that child plus the validator, and that is the pair a fresh
        // clone recorded at this line (`liveProcesses 2, expected 1`, log
        // /tmp/clash-qt-p4.w0Y8Uo/fresh-2-logs/make-test-integration.log). The
        // same instant produces either number depending only on how the
        // validator's few tens of milliseconds line up with the probe, which is
        // why the count was not the defect: its POSITION was.
        //
        // The gate below is the handoff itself, in the host's own published
        // terms, and it is strictly stronger than the settle it replaces:
        //
        //   * the incoming engine answered GET /version ITSELF, stamped with a
        //     generation newer than the one the refresh replaced;
        //   * it announced the generated controller address;
        //   * nothing is in flight - no validation child, no held launch, no
        //     configuration parse (isRestartPending(), lifecycle.h:133);
        //   * the ONLY configuration the host still has to keep is the
        //     refreshed one, so the retiring child and the validation candidate
        //     are both released (activeConfigPaths(), contract 5.1);
        //   * and the generation has stopped moving while Running and attached,
        //     which is what the previous version of this case waited for on its
        //     own. It was right about the generation - an earlier version
        //     captured the outgoing one and the incoming engine's
        //     managed-to-attached handoff (backend-r3 B2) then bumped it under
        //     the rejected-update assertion; the observed run is in
        //     /tmp/.../w02/logs/w02-real-diag.log, `states 1,2,3,1,2`,
        //     generation 4 where 3 was captured - but it ran AFTER the
        //     assertions that needed it.
        //
        // A refresh that never produced a new engine fails here, and it fails
        // naming the clause that stayed open.
        cb::Generation settled = generationBefore;
        QString gap;
        QVERIFY2(awaitReplacement(app, ready, generationBefore, launched.last(), readiness,
                                  &settled, &gap),
                 qPrintable(QStringLiteral("the refreshed engine never took over: %1. It was "
                                           "generation %2 before the refresh and is %3 now. "
                                           "%4 | readiness: %5 | logs: %6")
                                .arg(gap)
                                .arg(cb::number(generationBefore))
                                .arg(cb::number(app.backend->generation()))
                                .arg(report(app, subscription), ready.transcript(),
                                     coreLog(app).join(QLatin1Char('\n')))));
        QVERIFY2(cb::number(settled) > cb::number(generationBefore),
                 "a refresh that replaced the engine did not advance the generation");
        QVERIFY2(app.backend->state() == cb::CoreState::Running,
                 qPrintable(QStringLiteral("the pinned engine did not come back after a refresh. "
                                           "%1 | logs: %2")
                                .arg(report(app, subscription),
                                     coreLog(app).join(QLatin1Char('\n')))));

        // Now the file the REFRESHED engine was launched from, and the count.
        const QByteArray refreshedConfig = wf::readTextFile(launched.last());
        QVERIFY(refreshedConfig.contains("w02-real-v2"));
        QCOMPARE(topLevelScalar(refreshedConfig, "mixed-port"), QByteArray::number(mixedPort));
        QVERIFY2(app.backend->activeConfigPaths().contains(launched.last()),
                 qPrintable(QStringLiteral("the engine is running %1, not the refreshed %2")
                                .arg(app.backend->activeConfigPaths().join(QLatin1Char(' ')),
                                     launched.last())));
        // The configuration the FIRST engine was launched from is not something
        // anything still has to keep: the child that was running it is gone.
        QVERIFY2(!app.backend->activeConfigPaths().contains(live),
                 qPrintable(QStringLiteral("the superseded configuration %1 is still active")
                                .arg(live)));
        // One child - and because of the gate above this is now a statement
        // about the REPLACEMENT. The gate says which engine is live; this says
        // the one it replaced was not left behind, which is the part only the
        // operating system can answer.
        QCOMPARE(wf::liveProcessesOf(binary), 1);
        // Two engines came up over this journey, and the count above proves
        // only one of them is still running.
        QCOMPARE(ready.count(), 2);

        // ---- 5. an invalid update leaves the running engine alone ---------
        const QByteArray liveBytes = wf::readTextFile(profile.filePath);
        const cb::Generation before = settled;
        const int launchedBefore = launched.size();
        subscription.route("GET", kSubscriptionPath,
                           subscriptionReply(QByteArrayLiteral("note: not a profile\n"),
                                             quotaHeader(0, 0, 0, 0)));
        app.storeErrors.clear();
        app.profiles->updateProfile(uid);
        QVERIFY2(wf::waitFor([&app] { return !app.storeErrors.isEmpty(); }),
                 "an invalid update was accepted against the pinned engine");
        QVERIFY(wf::drainPast(500));
        QVERIFY(app.backend->drain());
        QCOMPARE(wf::readTextFile(profile.filePath), liveBytes);
        QCOMPARE(launched.size(), launchedBefore);
        QCOMPARE(app.backend->state(), cb::CoreState::Running);
        QCOMPARE(cb::number(app.backend->generation()), cb::number(before));
        QVERIFY(app.backend->activeConfigPaths().contains(launched.last()));
        QCOMPARE(wf::liveProcessesOf(binary), 1);
        QVERIFY2(app.events.coreFailures.empty(),
                 qPrintable(QStringLiteral("the pinned engine reported a failure: %1")
                                .arg(app.events.transcript())));

        // ---- 6. the quit leaves no child ---------------------------------
        QVERIFY2(app.quit(wf::deadlineFor(app.backend->timings().terminateWaitMs)),
                 qPrintable(QStringLiteral("the quit never completed: %1").arg(app.blockingReason())));
        QVERIFY2(app.shutdown->wasLastStopConfirmed(),
                 "the quit reported an unconfirmed stop of the pinned engine as a success");
        QVERIFY2(wf::awaitNoLiveProcess(binary),
                 "a source-built engine process outlived the application");
        QVERIFY(snapshots().isEmpty());
        QCOMPARE(app.unknownModuleEvents(), std::uint64_t(0));
        // Nothing in this arm CHANGED the machine's proxy. The one action that
        // is present is the Restore every quit performs unconditionally
        // (platform::SystemProxyService::shutdown() queues exactly one), and it
        // is asserted by name rather than allowed by a loose total: a second
        // Restore, or any Enable or Disable, would mean this arm reconfigured
        // the host, which it must never do.
        QVERIFY2(proxyLog->count(wf::ProxyAction::Restore) == 1 &&
                     proxyLog->count(wf::ProxyAction::Enable) == 0 &&
                     proxyLog->count(wf::ProxyAction::Disable) == 0 &&
                     proxyLog->total() == 1,
                 qPrintable(QStringLiteral("this arm touched the system proxy: %1")
                                .arg(proxyLog->transcript())));
        QVERIFY2(!subscription.sawUnexpectedRequest(),
                 qPrintable(subscription.redactedTranscript()));
    }

  private:
    /// The subscription body: DIRECT-only, TUN off, no remote node, and a
    /// marker that survives composition so "which body is live" can be read out
    /// of the generated file.
    /// It is also what the pinned engine is fed: wf::directOnlyProfile() adds
    /// nothing mihomo rejects (its config parser ignores unknown keys, so the
    /// marker survives) and nothing that would make the engine reach a network.
    static QByteArray body(const QString &marker) { return wf::directOnlyProfile(0, marker); }

    /// The user's own runtime overrides. `mixed-port` is a controller-owned
    /// field the store consumes, `mode` is an ordinary shallow override, and
    /// both must survive a subscription refresh.
    static QJsonObject overrides() {
        return QJsonObject{{QStringLiteral("mixed-port"), 27891},
                           {QStringLiteral("mode"), QStringLiteral("global")}};
    }

    QString engineDirectory() {
        const QString marker = environment_->filePath(QStringLiteral("engine/.keep"));
        const QString dir = QFileInfo(marker).absolutePath();
        QDir().mkpath(dir);
        return dir;
    }

    QStringList snapshots() const {
        return QDir(environment_->dataDir())
            .entryList({QStringLiteral(".runtime-*")}, QDir::Files | QDir::Hidden);
    }

    /// The generation the host reports once nothing is moving any more, or
    /// false on the budget.
    ///
    /// "Nothing is moving" is four things at once: the generation is at least
    /// `atLeast`, the engine is Running, this process is attached to its
    /// controller, and the generation has not changed across a quiet window
    /// with every queued module event drained. All four are needed because a
    /// replacement engine overlaps its predecessor: during a reload the OUTGOING
    /// generation is still Running and still attached, so "Running and
    /// connected" alone describes the engine that is going away. `atLeast` is
    /// what distinguishes them - a caller that expects a replacement passes the
    /// old generation plus one, and a caller that only wants the current one to
    /// stop moving passes 0.
    ///
    /// A budget expiry returns false rather than the last value seen: a settle
    /// that silently gives up would hand the caller exactly the stale number
    /// this function exists to avoid.
    ///
    /// WHY IT POLLS. Each round performs the liveness probe src/main.cpp's
    /// five-second QTimer performs (`poll.timeout -> bridge.refreshVersion()`),
    /// because wf::AssembledApp has no shell to perform it and, against a REAL
    /// engine, the host needs it after a reload. Observed, not assumed
    /// (/tmp/.../w02-fix/logs/w02-real-probe3.log): a reload replaces the
    /// engine at the SAME controller address, so
    /// core::MihomoClient::setEndpoint() takes its identical-endpoint branch
    /// and does NOT bump endpointEpoch_ - which leaves the requests already in
    /// flight to the replaced process "current". They then fail with
    /// "Connection refused" AFTER the replacement has answered, and each
    /// failure calls setConnected(false). The application recovers on its next
    /// poll; that log shows an explicit refreshVersion() clearing it at once,
    /// with the generation unchanged. Without the probe this wait is a coin
    /// toss (three of seven pinned-engine runs hit it), and a retry loop that
    /// merely waited longer would never clear it at all.
    static bool awaitSettledGeneration(wf::AssembledApp &app, std::uint64_t atLeast, int budgetMs,
                                       cb::Generation *out) {
        QElapsedTimer elapsed;
        elapsed.start();
        std::uint64_t previous = cb::number(app.backend->generation());
        while (elapsed.elapsed() < budgetMs) {
            app.backend->refreshVersion();
            if (!wf::drainPast(250)) return false;
            if (!app.backend->drain()) return false;
            const std::uint64_t now = cb::number(app.backend->generation());
            if (now == previous && now >= atLeast &&
                app.backend->state() == cb::CoreState::Running && app.backend->isConnected()) {
                *out = app.backend->generation();
                return true;
            }
            previous = now;
        }
        return false;
    }

    /// Which clause of the replacement handoff is still open, or empty when the
    /// refreshed engine has taken over.
    ///
    /// Every clause is something the HOST publishes about the work it is doing,
    /// not an inference from quiet time: a readiness the incoming engine
    /// announced under its own generation, the restart-pending flag the reload
    /// gate itself consults, and the retention set contract section 5.1
    /// defines. `expected` is the configuration the refresh generated, and the
    /// retention set is compared to it EXACTLY - a retiring child's
    /// configuration, a validation candidate and a held launch all show up
    /// there (core_process.cpp:587-595), so "exactly this one" is the statement
    /// that none of them is left.
    static QString replacementGap(wf::AssembledApp &app, const ReadyLedger &ready,
                                  cb::Generation replaced, const QString &expected) {
        const std::optional<ReadyLedger::Entry> latest = ready.readyAfter(replaced);
        if (!latest)
            return QStringLiteral("no engine newer than generation %1 has answered GET /version, "
                                  "so this process is still talking to the one the refresh "
                                  "replaces (readiness: %2)")
                .arg(cb::number(replaced))
                .arg(ready.transcript());
        if (latest->endpoint.port != wf::kGeneratedControllerPort)
            return QStringLiteral("the replacement announced port %1, not the generated "
                                  "controller address %2")
                .arg(latest->endpoint.port)
                .arg(wf::kGeneratedControllerPort);
        if (app.backend->isRestartPending())
            return QStringLiteral("a validation child, a held launch or a configuration parse is "
                                  "still outstanding");
        const QStringList active = app.backend->activeConfigPaths();
        if (active != QStringList{expected})
            return QStringLiteral("the host still has to keep [%1]; the refreshed configuration "
                                  "alone is %2")
                .arg(active.join(QLatin1Char(' ')), expected);
        return {};
    }

    /// Waits for the refreshed engine to take over, and reports the generation
    /// it settled on.
    ///
    /// The quiet window, the liveness probe and the Running/attached conditions
    /// are awaitSettledGeneration()'s, for the reasons documented there - the
    /// same-address reload that leaves a stale "Connection refused" behind is
    /// exactly what a refresh produces. What this adds is WHICH engine those
    /// conditions are about: the settle alone is satisfied by a generation that
    /// merely stopped moving, and replacementGap() is what says the engine it
    /// stopped moving on is the incoming one. `*gap` carries the last open
    /// clause out for the failure message, so a budget expiry names the thing
    /// that never happened.
    static bool awaitReplacement(wf::AssembledApp &app, const ReadyLedger &ready,
                                 cb::Generation replaced, const QString &expected, int budgetMs,
                                 cb::Generation *out, QString *gap) {
        QElapsedTimer elapsed;
        elapsed.start();
        std::uint64_t previous = cb::number(app.backend->generation());
        *gap = QStringLiteral("the replacement never started");
        while (elapsed.elapsed() < budgetMs) {
            app.backend->refreshVersion();
            if (!wf::drainPast(250)) return false;
            if (!app.backend->drain()) return false;
            const std::uint64_t now = cb::number(app.backend->generation());
            *gap = replacementGap(app, ready, replaced, expected);
            if (gap->isEmpty() && now == previous && now > cb::number(replaced) &&
                app.backend->state() == cb::CoreState::Running && app.backend->isConnected()) {
                *out = app.backend->generation();
                return true;
            }
            previous = now;
        }
        return false;
    }

    static QStringList coreLog(wf::AssembledApp &app) {
        QStringList lines;
        for (const QString &line : app.events.logLines) lines << line;
        return lines;
    }

    static QString report(wf::AssembledApp &app, LoopbackServer &server) {
        return QStringLiteral("state=%1 connected=%2 | events: %3 | store: %4 | %5")
            .arg(static_cast<int>(app.backend->state()))
            .arg(app.backend->isConnected())
            .arg(app.events.transcript(), app.storeErrors.join(QLatin1Char(' ')),
                 server.pendingReport());
    }

    static QString portSkipReason(const wf::ControllerRelay &relay) {
        return QStringLiteral(
                   "W02 needs the generated controller address 127.0.0.1:%1, which is in use: %2. "
                   "A running clash-qt core is the usual reason. Not asserted rather than "
                   "asserted weakly.")
            .arg(wf::kGeneratedControllerPort)
            .arg(relay.errorString());
    }

    std::unique_ptr<ScopedEnvironment> environment_;
    QString enginePath_;
};

QTEST_GUILESS_MAIN(W02SubscriptionUpdateTest)
#include "w02_subscription_update_test.moc"
