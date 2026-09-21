// The real backend driving the REAL, locally built engine.
//
// Separate evidence from the fake-backed and fixture-backed suites: those prove
// the contract is implementable and that this implementation honours it; only
// this one proves the ENGINE does. Label: real-core.
//
// It refuses to run against anything but the engine this project built. There is
// no discovery here and no fallback: CLASH_QT_CORE_BINARY, or a skip.
//
// It does not touch the system proxy, the user's network, or any port but the
// two ephemeral ones it claims itself.

#include <QtTest>

#include <QFile>
#include <QTcpServer>

#include "core/mihomo/mihomo_backend.h"
#include "support/scoped_environment.h"

namespace cb = core::backend;

namespace {

class Watcher final : public cb::BackendObserver {
  public:
    std::vector<cb::Endpoint> ready;
    std::vector<cb::Completion> failures;
    QStringList logLines;
    QString version;
    QString mode;
    int rules = -1;
    bool sawStopCompleted = false;
    bool stopConfirmed = false;

    void coreReady(const cb::Completion &, const cb::Endpoint &endpoint) noexcept override {
        ready.push_back(endpoint);
    }
    void coreLogLine(cb::Generation, const QString &line) noexcept override {
        logLines.append(line);
    }
    void coreFailed(const cb::Completion &completion) noexcept override {
        failures.push_back(completion);
    }
    void stopCompleted(const cb::StopCompleted &result) noexcept override {
        sawStopCompleted = true;
        stopConfirmed = result.confirmed;
    }
    void versionReceived(const cb::Completion &completion, const QString &value) noexcept override {
        if (completion.isOk() && !value.isEmpty()) version = value;
    }
    void configReceived(const cb::Completion &completion, const cb::BaseConfig &config) noexcept override {
        if (completion.isOk()) mode = config.mode;
    }
    void rulesUpdated(const cb::Completion &completion, cb::Span<cb::Rule> list) noexcept override {
        if (completion.isOk()) rules = static_cast<int>(list.size());
    }
};

quint16 claimFreePort() {
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost)) return 0;
    const quint16 port = probe.serverPort();
    probe.close();
    return port;
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
    }
    void cleanup() { environment_.reset(); }

    void theRealEngineIsSupervisedThroughThePublishedContract() {
        const quint16 controllerPort = claimFreePort();
        const quint16 mixedPort = claimFreePort();
        QVERIFY(controllerPort != 0 && mixedPort != 0);
        QVERIFY(controllerPort != mixedPort);

        // Deliberately minimal and inert: direct mode, no TUN, no providers, and
        // a listener on a port this test claimed. Nothing here reaches the
        // developer's network or system proxy settings.
        const QString config = environment_->filePath(QStringLiteral("real-core.yaml"));
        QFile file(config);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("mode: direct\n"
                   "log-level: info\n"
                   "ipv6: false\n"
                   "allow-lan: false\n"
                   "mixed-port: " + QByteArray::number(mixedPort) + "\n"
                   "external-controller: 127.0.0.1:" + QByteArray::number(controllerPort) + "\n"
                   "secret: modcore-real-core\n"
                   "rules:\n  - MATCH,DIRECT\n");
        file.close();

        core::MihomoBackendImpl backend;
        Watcher watcher;
        QVERIFY(backend.addObserver(&watcher));

        // G1: the managed path is the staged engine or nothing. Here it is the
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
        QVERIFY(!cb::isValid(backend.managedEndpoint()));
        QVERIFY(backend.removeObserver(&watcher));
    }

  private:
    QString engine_;
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(BackendRealCoreTest)
#include "backend_real_core_test.moc"
