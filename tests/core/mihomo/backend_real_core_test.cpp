// The real backend driving the REAL, locally built engine.
//
// Separate evidence from the fake-backed and fixture-backed suites: those prove
// the contract is implementable and that this implementation honours it; only
// this one proves the ENGINE does. Label: real-core (and integration, so
// `make test-integration` selects it).
//
// It refuses to run against anything but the engine this project built. There is
// no discovery here and no fallback: CLASH_QT_CORE_BINARY, or a skip.
//
// It does not touch the system proxy, the user's network, or any port but the
// ephemeral ones it claims itself.
//
// WHY THIS FILE GREW (backend-r3 B4)
//
// Section 10 claims the real backend "additionally passes against the locally
// built mihomo". Before r3 that claim rested on backend-real-contract, which
// despite its name drives clash-qt-fake-core, plus one smoke case here. A suite
// whose name merely contains "real" is not evidence. The acceptance bullets that
// the real ENGINE can honestly drive are therefore asserted here, against
// ${CMAKE_BINARY_DIR}/core/mihomo, and the ones it cannot are named below with
// the reason - because a bullet only the fixture protects is a bullet the engine
// does not have to honour.
//
// Driven here, against the real engine:
//   #1  detach leaves an attached controller running; stop terminates only a
//       managed core                       -> detachAndStopReachOnlyWhatWeStarted
//   #2  a superseded completion is MARKED, and compares older
//   #3  the generation is bumped before the abort (positive arm)
//                                          -> aCompletionAbortedByAnEndpointChange...
//   #4  readiness is 200 + a string version, not process start (positive arm)
//                                          -> theRealEngineIsSupervised...
//   #6  a validation failure leaves the running configuration intact
//                                          -> aValidationFailureLeavesTheRealEngineRunning
//   #9  confirmed == true is reported as such, and B1's stamping rule holds
//                                          -> theRealEngineIsSupervised...
//   #10 no observer is invoked re-entrantly from within a mutating call
//   #11 removing an observer during delivery is safe
//                                          -> deliveryIsNeverReEntrant...
//
// NOT drivable by the real engine, and why. Each stays on the loopback/fixture
// suite, which is where the hazard can actually be produced:
//   #3, negative arm - proving "abort before bump" delivers a stale completion
//       requires BUILDING that hazard. A production backend must not be able to
//       exhibit it; only the fake's setAbortOrdering(AbortThenBump) can.
//   #4, negative arms - mihomo's /version always answers 200 with a string
//       version. 500, 404, a missing field, a numeric version and a non-object
//       body cannot be produced without a scripted controller.
//   #5  - the readiness budget needs a core that comes up, keeps logging and
//       never answers /version. A real mihomo that starts answers /version in
//       milliseconds; one that does not, does not log either. Neither the idle
//       refresh nor the hard cap can be exercised without FakeCore's gates.
//   #7  - "the pending launch is consumed only after the prior exit" needs the
//       prior exit to be SLOW enough to observe. mihomo honours SIGTERM and is
//       gone in milliseconds; FakeCore::ignoresTerminate() is what makes the
//       window real rather than a race.
//   #8  - a cancelled validation keeps its config path active until it exits.
//       mihomo's `-t` runs to completion and cannot be parked, so there is no
//       dying-validator window to observe.
//   #9, unconfirmed arm - confirmed == false is produced by the PRIVILEGED
//       SERVICE disconnecting mid-stop. It is a property of the helper, not of
//       the engine; a direct child's exit is always observed.
//   section 5.2 - cancelling a readiness probe needs the probe held in flight,
//       which needs a controller that refuses to answer.
//   B2's TUN supersession - drivable in principle, deliberately not driven:
//       the PATCH that follows would ask the engine to create a TUN device on
//       the developer's machine. This suite does not touch the user's network.

#include <QtTest>

#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>

#include "core/mihomo/mihomo_backend.h"
#include "support/scoped_environment.h"

namespace cb = core::backend;

namespace {

class Watcher final : public cb::BackendObserver {
  public:
    Watcher() = default;
    explicit Watcher(core::MihomoBackendImpl *backend) : backend_(backend) {}

    struct CompletionEvent {
        QString channel;
        cb::Completion completion;
        QString payload;
    };

    std::vector<cb::Endpoint> ready;
    std::vector<cb::Completion> failures;
    std::vector<CompletionEvent> events;
    std::vector<cb::StopCompleted> stops;
    QStringList logLines;
    QString version;
    QString mode;
    int rules = -1;
    int callbackCount = 0;
    bool sawStopCompleted = false;
    bool stopConfirmed = false;
    bool sawReentrantDelivery = false;
    std::function<void()> onNextEvent;

    cb::Generation lastObserved() const { return lastObserved_; }
    const CompletionEvent *find(const QString &channel, cb::RequestId request) const {
        for (const auto &event : events)
            if (event.channel == channel && event.completion.request == request) return &event;
        return nullptr;
    }

    void coreStateChanged(cb::Generation generation, cb::CoreState, cb::Ownership) noexcept override {
        note(generation);
    }
    void coreReady(const cb::Completion &completion, const cb::Endpoint &endpoint) noexcept override {
        note(completion.generation);
        ready.push_back(endpoint);
    }
    void coreLogLine(cb::Generation generation, const QString &line) noexcept override {
        note(generation);
        logLines.append(line);
    }
    void coreFailed(const cb::Completion &completion) noexcept override {
        note(completion.generation);
        failures.push_back(completion);
    }
    void coreStopped(cb::Generation generation) noexcept override { note(generation); }
    void stopCompleted(const cb::StopCompleted &result) noexcept override {
        note(result.generation);
        sawStopCompleted = true;
        stopConfirmed = result.confirmed;
        stops.push_back(result);
    }
    void endpointChanged(cb::Generation generation, const cb::Endpoint &,
                         cb::Ownership) noexcept override {
        note(generation);
    }
    void connectedChanged(cb::Generation generation, bool) noexcept override { note(generation); }
    void versionReceived(const cb::Completion &completion, const QString &value) noexcept override {
        record(QStringLiteral("version"), completion, value);
        if (completion.isOk() && !value.isEmpty()) version = value;
    }
    void configReceived(const cb::Completion &completion, const cb::BaseConfig &config) noexcept override {
        record(QStringLiteral("config"), completion, config.mode);
        if (completion.isOk()) mode = config.mode;
    }
    void modeChanged(const cb::Completion &completion, const QString &value) noexcept override {
        record(QStringLiteral("mode"), completion, value);
    }
    void rulesUpdated(const cb::Completion &completion, cb::Span<cb::Rule> list) noexcept override {
        record(QStringLiteral("rules"), completion, QString::number(list.size()));
        if (completion.isOk()) rules = static_cast<int>(list.size());
    }
    void proxiesUpdated(const cb::Completion &completion, cb::Span<cb::ProxyGroup> groups,
                        cb::Span<cb::ProxyNode>) noexcept override {
        record(QStringLiteral("proxies"), completion, QString::number(groups.size()));
    }
    void errorOccurred(const cb::Completion &completion) noexcept override {
        note(completion.generation);
        failures.push_back(completion);
    }

  private:
    void note(cb::Generation generation) {
        ++callbackCount;
        // The section 7 obligation, enforced rather than assumed.
        if (backend_ && backend_->isInsideMutatingCall()) sawReentrantDelivery = true;
        if (generation > lastObserved_) lastObserved_ = generation;
        if (onNextEvent) {
            const auto once = onNextEvent;
            onNextEvent = nullptr;
            once();
        }
    }
    void record(const QString &channel, const cb::Completion &completion, const QString &payload) {
        note(completion.generation);
        events.push_back({channel, completion, payload});
    }

    core::MihomoBackendImpl *backend_ = nullptr;
    cb::Generation lastObserved_ = cb::Generation::Initial;
};

quint16 claimFreePort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost)) return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
}

QByteArray inertConfig(quint16 controllerPort, quint16 mixedPort, const QByteArray &secret,
                       const QByteArray &extraRules = {}) {
    // Deliberately minimal and inert: direct mode, no TUN, no providers, and a
    // listener on a port the test claimed. Nothing here reaches the developer's
    // network or system proxy settings.
    return "mode: direct\n"
           "log-level: info\n"
           "ipv6: false\n"
           "allow-lan: false\n"
           "mixed-port: " + QByteArray::number(mixedPort) + "\n"
           "external-controller: 127.0.0.1:" + QByteArray::number(controllerPort) + "\n"
           "secret: " + secret + "\n"
           "rules:\n" + extraRules + "  - MATCH,DIRECT\n";
}

/// A real mihomo THIS TEST owns. The backend never started it and must never
/// terminate it - which is exactly what section 1's "attached controller" means,
/// and what makes the second half of acceptance bullet 1 assertable at all.
class ExternalEngine {
  public:
    ExternalEngine(QString binary, QString workDir, QString configPath)
        : binary_(std::move(binary)), workDir_(std::move(workDir)),
          configPath_(std::move(configPath)) {}
    ~ExternalEngine() { stop(); }

    ExternalEngine(const ExternalEngine &) = delete;
    ExternalEngine &operator=(const ExternalEngine &) = delete;

    bool start() {
        controllerPort_ = claimFreePort();
        const quint16 mixedPort = claimFreePort();
        if (controllerPort_ == 0 || mixedPort == 0 || controllerPort_ == mixedPort) return false;
        QFile file(configPath_);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        file.write(inertConfig(controllerPort_, mixedPort, secret_.toUtf8()));
        file.close();
        process_.setProcessChannelMode(QProcess::MergedChannels);
        process_.start(binary_, {"-d", workDir_, "-f", configPath_});
        if (!process_.waitForStarted(15000)) return false;
        return waitUntilListening();
    }

    bool isRunning() const { return process_.state() == QProcess::Running; }
    void stop() {
        if (process_.state() == QProcess::NotRunning) return;
        process_.terminate();
        if (!process_.waitForFinished(5000)) {
            process_.kill();
            process_.waitForFinished(5000);
        }
    }

    cb::Endpoint endpoint() const {
        cb::Endpoint endpoint;
        endpoint.host = QStringLiteral("127.0.0.1");
        endpoint.port = controllerPort_;
        endpoint.secret = secret_;
        return endpoint;
    }
    QString output() { return QString::fromUtf8(process_.readAll()); }

  private:
    bool waitUntilListening() {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < 30000) {
            if (process_.state() != QProcess::Running) return false;
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, controllerPort_);
            if (socket.waitForConnected(200)) {
                socket.abort();
                return true;
            }
            QTest::qWait(100);
        }
        return false;
    }

    QString binary_;
    QString workDir_;
    QString configPath_;
    QString secret_ = QStringLiteral("modcore-external");
    quint16 controllerPort_ = 0;
    QProcess process_;
};

/// A controller is reachable only when it has ANSWERED, and attach() fetches the
/// snapshot set exactly once. Re-issuing inside QTRY_VERIFY is what turns "the
/// port is open" into "the engine is serving", without a sleep.
bool connectedAfterRefresh(core::MihomoBackendImpl &backend) {
    if (backend.isConnected()) return true;
    backend.refreshVersion();
    return backend.isConnected();
}

}  // namespace

class BackendRealCoreTest : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        engine_ = qEnvironmentVariable("CLASH_QT_CORE_BINARY");
        if (engine_.isEmpty() || !QFileInfo(engine_).isExecutable())
            QSKIP("CLASH_QT_CORE_BINARY does not name the locally built engine");
    }

    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("rc"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
        QVERIFY2(environment_->realPreferencesUnchanged(),
                 "the developer's own preferences changed before this test even ran");
    }
    void cleanup() {
        QVERIFY2(environment_->realPreferencesUnchanged(),
                 "this test wrote into the developer's own preference store");
        environment_.reset();
    }

    // --- #4 (positive arm), #9 (confirmed arm), B1's stamping rule, and the
    //     engine-provenance claim. The bullet numbers index this file's own
    //     table above.

    void theRealEngineIsSupervisedThroughThePublishedContract() {
        const quint16 controllerPort = claimFreePort();
        const quint16 mixedPort = claimFreePort();
        QVERIFY(controllerPort != 0 && mixedPort != 0);
        QVERIFY(controllerPort != mixedPort);

        const QString config = writeConfig(QStringLiteral("real-core.yaml"), controllerPort,
                                           mixedPort);
        QVERIFY(!config.isEmpty());

        core::MihomoBackendImpl backend;
        Watcher watcher(&backend);
        QVERIFY(backend.addObserver(&watcher));

        // The managed path resolves the engine this project staged or nothing -
        // never a PATH lookup or another Clash installation. Here it is the
        // locally built one, and it is REPORTED - label and provenance both.
        backend.setBinaryPath(engine_);
        QCOMPARE(backend.binaryPath(), engine_);

        const cb::RequestId launch = backend.start(config, environment_->dataDir());
        QVERIFY(launch != cb::RequestId::Invalid);

        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Running ||
                                     backend.state() == cb::CoreState::Failed,
                                 60000);
        if (backend.state() == cb::CoreState::Failed) {
            QVERIFY(backend.drainPendingEvents());
            QFAIL(qPrintable(QStringLiteral("the real engine did not come up: %1\n%2")
                                 .arg(watcher.failures.empty()
                                          ? QStringLiteral("(no reason reported)")
                                          : watcher.failures.front().error.message,
                                      watcher.logLines.join(QLatin1Char('\n')))));
        }

        QVERIFY(backend.drainPendingEvents());
        // Readiness means GET /version answered 200 with a string version - and
        // the real engine is what answered it.
        QCOMPARE(static_cast<int>(watcher.ready.size()), 1);
        QCOMPARE(watcher.ready.front().port, controllerPort);
        QCOMPARE(backend.managedEndpoint().port, controllerPort);
        QCOMPARE(backend.ownership(), cb::Ownership::Managed);

        // Whatever engine was resolved must be reported. The log channel carries
        // the line, and it names the engine and its recorded provenance.
        bool reportedTheEngine = false;
        for (const QString &line : std::as_const(watcher.logLines))
            if (line.startsWith(QStringLiteral("engine ")) && line.contains(engine_))
                reportedTheEngine = true;
        QVERIFY2(reportedTheEngine, "the resolved engine was not reported");

        // Attach to the core we started: the two are independently observable,
        // and attachmentOwnership() answers "is this our own child?" directly.
        backend.attach(backend.managedEndpoint());
        QTRY_VERIFY_WITH_TIMEOUT(backend.isConnected(), 15000);
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Managed);
        QVERIFY2(!backend.isExternalControllerConnected(),
                 "our own managed child was reported as an external controller");

        QTRY_VERIFY_WITH_TIMEOUT(!watcher.version.isEmpty(), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(watcher.mode == QStringLiteral("direct"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(watcher.rules >= 1, 15000);
        qInfo().noquote() << "real engine version:" << watcher.version
                          << " rules:" << watcher.rules;

        const cb::RequestId stopRequest = backend.stop();
        QVERIFY(stopRequest != cb::RequestId::Invalid);
        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Stopped, 20000);
        QVERIFY(backend.drainPendingEvents());
        QVERIFY2(watcher.sawStopCompleted, "stop() produced no terminal answer");
        QVERIFY2(watcher.stopConfirmed, "a direct child's exit was not confirmed");
        QCOMPARE(static_cast<int>(watcher.stops.size()), 1);
        // backend-r3 B2: the outcome is stated on the completion, not inferred.
        QCOMPARE(watcher.stops.front().status, cb::CompletionStatus::Ok);
        // backend-r3 B1: a stop's terminal answer carries the generation current
        // after every bump its own teardown caused, so a consumer applying
        // section 2's mandatory rejection rule can never drop it.
        QVERIFY2(!cb::isSuperseded(watcher.stops.front().generation, watcher.lastObserved()),
                 "the real engine's stop completion compared older than the newest "
                 "observed generation (backend-r3 B1)");
        QVERIFY(!cb::isValid(backend.managedEndpoint()));
        QVERIFY(backend.removeObserver(&watcher));
    }

    // --- 1, both halves, with a managed real engine and an external real engine
    //     alive at the same moment.

    void detachAndStopReachOnlyWhatThisComponentStarted() {
        ExternalEngine external(engine_, environment_->dataDir(),
                                environment_->filePath(QStringLiteral("external.yaml")));
        QVERIFY2(external.start(), "the external engine did not come up");

        core::MihomoBackendImpl backend;
        Watcher watcher(&backend);
        QVERIFY(backend.addObserver(&watcher));
        backend.setBinaryPath(engine_);

        // 1a. Detaching from a controller this component did not start must
        //     never terminate it.
        backend.attach(external.endpoint());
        QTRY_VERIFY_WITH_TIMEOUT(connectedAfterRefresh(backend), 30000);
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
        QVERIFY(backend.isExternalControllerConnected());

        backend.detach();
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(!backend.isAttached());
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::None);
        QVERIFY2(external.isRunning(), "detach() killed a controller it did not start");

        // Re-attaching proves the engine is still SERVING, not merely alive.
        backend.attach(external.endpoint());
        QTRY_VERIFY_WITH_TIMEOUT(connectedAfterRefresh(backend), 30000);

        // 1b. Now a managed core as well. Two engines, one of each ownership,
        //     alive at the same time - which is what makes "stop terminates ONLY
        //     a managed core" assertable instead of vacuous.
        const quint16 controllerPort = claimFreePort();
        const quint16 mixedPort = claimFreePort();
        QVERIFY(controllerPort != 0 && mixedPort != 0 && controllerPort != mixedPort);
        const QString config = writeConfig(QStringLiteral("managed.yaml"), controllerPort,
                                           mixedPort);
        backend.start(config, environment_->dataDir());
        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Running ||
                                     backend.state() == cb::CoreState::Failed,
                                 60000);
        QCOMPARE(backend.state(), cb::CoreState::Running);

        // The managed start bumped the generation, which dropped the attachment's
        // connected flag; re-establish it so both are demonstrably live.
        backend.attach(external.endpoint());
        QTRY_VERIFY_WITH_TIMEOUT(connectedAfterRefresh(backend), 30000);
        QCOMPARE(backend.ownership(), cb::Ownership::Managed);
        QCOMPARE(backend.managedEndpoint().port, controllerPort);
        QCOMPARE(backend.attachmentOwnership(), cb::Ownership::Attached);
        QVERIFY2(backend.isExternalControllerConnected(),
                 "an external controller was not reported as external while a managed "
                 "core was running");

        const cb::RequestId stopRequest = backend.stop();
        QVERIFY(stopRequest != cb::RequestId::Invalid);

        // The managed child is gone - Stopped is reached only when its exit is
        // observed - and the stop is reported confirmed.
        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Stopped, 20000);
        QVERIFY(backend.drainPendingEvents());
        QVERIFY(!watcher.stops.empty());
        QVERIFY(watcher.stops.back().confirmed);
        QVERIFY(!cb::isValid(backend.managedEndpoint()));

        // And the engine this component never started is untouched: still
        // running, still attached, and still answering.
        QVERIFY2(external.isRunning(),
                 "stop() terminated an engine this component never started");
        QVERIFY2(backend.isAttached(),
                 "stop() detached a controller this component never started");
        QCOMPARE(backend.currentEndpoint().port, external.endpoint().port);
        const cb::RequestId probe = backend.refreshVersion();
        QVERIFY(probe != cb::RequestId::Invalid);
        QTRY_VERIFY_WITH_TIMEOUT(watcher.find(QStringLiteral("version"), probe) != nullptr, 20000);
        const auto *answered = watcher.find(QStringLiteral("version"), probe);
        QVERIFY2(answered->completion.isOk(),
                 "the external engine stopped answering after a stop that was "
                 "supposed to reach only the managed core");
        QVERIFY(!answered->payload.isEmpty());
        QVERIFY(backend.removeObserver(&watcher));
    }

    // --- 2 and 3 (positive arm). A completion the endpoint change abandoned is
    //     MARKED superseded and carries the generation it was submitted under.

    void aCompletionAbortedByAnEndpointChangeIsMarkedSuperseded() {
        ExternalEngine external(engine_, environment_->dataDir(),
                                environment_->filePath(QStringLiteral("external.yaml")));
        QVERIFY2(external.start(), "the external engine did not come up");

        core::MihomoBackendImpl backend;
        Watcher watcher(&backend);
        QVERIFY(backend.addObserver(&watcher));
        backend.attach(external.endpoint());
        QTRY_VERIFY_WITH_TIMEOUT(connectedAfterRefresh(backend), 30000);

        const cb::RequestId version = backend.refreshVersion();
        QVERIFY(version != cb::RequestId::Invalid);
        const cb::Generation submitted = backend.generation();

        // The endpoint changes in the SAME event-loop turn, so the request the
        // line above issued to the real engine cannot yet have been answered:
        // finished() is delivered from the event loop, which has not run. No
        // sleep, no race. The replacement endpoint is a port nothing listens on;
        // what matters is that the endpoint CHANGED.
        const quint16 vacant = claimFreePort();
        QVERIFY(vacant != 0);
        cb::Endpoint elsewhere;
        elsewhere.host = QStringLiteral("127.0.0.1");
        elsewhere.port = vacant;
        backend.attach(elsewhere);

        // Section 2's ordering rule: the bump precedes the abort, so the abort -
        // whose finished() may run synchronously inside it - cannot deliver a
        // completion stamped with the generation that invalidated it.
        QVERIFY2(backend.generation() > submitted,
                 "the endpoint change did not bump the generation");
        QVERIFY(backend.drainPendingEvents());
        QTRY_VERIFY_WITH_TIMEOUT(watcher.find(QStringLiteral("version"), version) != nullptr, 20000);

        const auto *completion = watcher.find(QStringLiteral("version"), version);
        QVERIFY2(completion->completion.status == cb::CompletionStatus::Superseded,
                 "an abandoned completion was not MARKED superseded (backend-r2 A2)");
        QCOMPARE(completion->completion.generation, submitted);
        QVERIFY2(cb::isSuperseded(completion->completion.generation, watcher.lastObserved()),
                 "the completion did not compare older than the newest observed generation");
        QVERIFY2(completion->payload.isEmpty(),
                 "a completion aborted by an endpoint change was delivered as live data");
        QVERIFY(external.isRunning());
        QVERIFY(backend.removeObserver(&watcher));
    }

    // --- 6. A validation failure leaves the running configuration intact, with
    //        the real `mihomo -t` doing the rejecting.

    void aValidationFailureLeavesTheRealEngineRunning() {
        const quint16 controllerPort = claimFreePort();
        const quint16 mixedPort = claimFreePort();
        QVERIFY(controllerPort != 0 && mixedPort != 0 && controllerPort != mixedPort);

        core::MihomoBackendImpl backend;
        Watcher watcher(&backend);
        QVERIFY(backend.addObserver(&watcher));
        backend.setBinaryPath(engine_);

        const QString running = writeConfig(QStringLiteral("running.yaml"), controllerPort,
                                            mixedPort);
        backend.start(running, environment_->dataDir());
        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Running ||
                                     backend.state() == cb::CoreState::Failed,
                                 60000);
        QCOMPARE(backend.state(), cb::CoreState::Running);
        QCOMPARE(backend.activeConfigPaths(), QStringList{running});

        // The real engine rejects this: "unsupported rule type". The candidate is
        // validated by a SEPARATE child while the running core stays live.
        const quint16 candidatePort = claimFreePort();
        const quint16 candidateMixed = claimFreePort();
        QVERIFY(candidatePort != 0 && candidateMixed != 0);
        const QString candidate = writeConfig(QStringLiteral("candidate.yaml"), candidatePort,
                                              candidateMixed,
                                              "  - NOT-A-REAL-RULE-TYPE,example.invalid,DIRECT\n");
        backend.start(candidate, environment_->dataDir());
        QCOMPARE(backend.state(), cb::CoreState::Running);
        QVERIFY(backend.activeConfigPaths().contains(running));
        QVERIFY(backend.activeConfigPaths().contains(candidate));

        QTRY_VERIFY_WITH_TIMEOUT(!backend.isRestartPending(), 60000);
        QVERIFY(backend.drainPendingEvents());
        QVERIFY2(backend.state() == cb::CoreState::Running,
                 "a rejected candidate took the running engine down with it");
        QCOMPARE(backend.activeConfigPaths(), QStringList{running});
        QCOMPARE(backend.managedEndpoint().port, controllerPort);
        QVERIFY(!watcher.failures.empty());
        bool reportedValidationFailure = false;
        for (const cb::Completion &failure : std::as_const(watcher.failures))
            if (failure.error.code == cb::ErrorCode::ValidationFailed) reportedValidationFailure = true;
        QVERIFY2(reportedValidationFailure,
                 "the real engine's rejection was not reported as ValidationFailed");

        backend.stop();
        QTRY_VERIFY_WITH_TIMEOUT(backend.state() == cb::CoreState::Stopped, 20000);
        QVERIFY(backend.removeObserver(&watcher));
    }

    // --- 10 and 11, against events the real engine produced.

    void deliveryIsNeverReEntrantAndObserverRemovalIsSafe() {
        ExternalEngine external(engine_, environment_->dataDir(),
                                environment_->filePath(QStringLiteral("external.yaml")));
        QVERIFY2(external.start(), "the external engine did not come up");

        core::MihomoBackendImpl backend;
        Watcher first(&backend);
        Watcher second(&backend);
        Watcher third(&backend);
        QVERIFY(backend.addObserver(&first));
        QVERIFY(backend.addObserver(&second));
        QVERIFY(backend.addObserver(&third));
        QVERIFY(!backend.addObserver(&first));
        QVERIFY(!backend.addObserver(nullptr));

        // Removing observers from inside a delivery, which is the case the
        // contract calls out as having to be safe.
        second.onNextEvent = [&] {
            backend.removeObserver(&second);
            backend.removeObserver(&third);
        };

        backend.attach(external.endpoint());
        QTRY_VERIFY_WITH_TIMEOUT(connectedAfterRefresh(backend), 30000);
        QVERIFY(!first.sawReentrantDelivery);

        // Every mutating path, including the one that emits five signals from
        // inside a single call (MihomoClient::clearLiveState).
        backend.setMode(QStringLiteral("global"));
        backend.refreshVersion();
        backend.refreshProxies();
        backend.refreshRules();
        backend.refreshConfig();
        backend.detach();  // clears live state
        backend.attach(external.endpoint());
        QTRY_VERIFY_WITH_TIMEOUT(connectedAfterRefresh(backend), 30000);
        QVERIFY(backend.drainPendingEvents());
        QTest::qWait(100);
        QVERIFY(backend.drainPendingEvents());

        QVERIFY2(!first.sawReentrantDelivery,
                 "an observer was invoked from inside a mutating call");
        QVERIFY2(first.callbackCount > 20,
                 "the observer received almost nothing: the assertion above is vacuous");
        QCOMPARE(second.callbackCount, 1);
        QVERIFY2(third.callbackCount == 0,
                 "an observer removed during delivery was invoked afterwards");
        QVERIFY(!backend.removeObserver(&second));
        QVERIFY(backend.removeObserver(&first));
        QVERIFY(external.isRunning());
    }

  private:
    QString writeConfig(const QString &name, quint16 controllerPort, quint16 mixedPort,
                        const QByteArray &extraRules = {}) {
        const QString path = environment_->filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        file.write(inertConfig(controllerPort, mixedPort, "modcore-real-core", extraRules));
        file.close();
        return path;
    }

    QString engine_;
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(BackendRealCoreTest)
#include "backend_real_core_test.moc"
