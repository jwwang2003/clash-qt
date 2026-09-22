// Core process execution: restart without blocking the GUI thread, the privileged
// service lease and its stop acknowledgement, cancelable validation, and probe
// behaviour against an unresponsive controller.
//
// Partition of the former `runtime` suite (tests/core/runtime_test.cpp). Cases are
// carried over verbatim; see tests/README.md for the full original-to-new map.
//
// The privileged-service case drives platform::PrivilegedServiceClient as well,
// but its oracle throughout is core::CoreProcess::state(), so it belongs here and
// not in the platform lane.
#include <QtTest>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QtEndian>
#include <chrono>
#include <functional>
#include <memory>

#include "core/mihomo/process/core_process.h"
#include "app/composition/privileged_service_adapter.h"
#include "platform/service/privileged_service_client.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"

class CoreProcessTest : public QObject {
    Q_OBJECT
private slots:
    // One scoped environment per test function, as the original suite had one
    // QTemporaryDir per test function. ScopedEnvironment additionally *restores*
    // any CLASH_QT_DATA_DIR the developer had exported, where the original
    // cleanup() unset it and lost it for the rest of the process.
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("core"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() { environment_.reset(); }
    void restartWaitsForOldExitWithoutBlockingGui() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake core process");
#else
        const QString binary = environment_->filePath("stubborn-core");
        testsupport::writeFile(binary, "#!/usr/bin/python3\nimport os,sys,signal,time\n"
            "if '-t' in sys.argv: sys.exit(0)\n"
            "marker=os.path.join(sys.argv[sys.argv.index('-d')+1], 'started-once')\n"
            "if not os.path.exists(marker):\n signal.signal(signal.SIGTERM,signal.SIG_IGN)\n open(marker,'w').close()\n"
            "print('started-child',flush=True)\ntime.sleep(30)\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        const QString config = environment_->filePath("config.yaml");
        testsupport::writeFile(config, "external-controller: 127.0.0.1:" + QByteArray::number(server.serverPort()) + "\n");
        core::CoreProcess process;
        process.setBinaryPath(binary);
        QSignalSpy lines(&process, &core::CoreProcess::logLine);
        QSignalSpy stopped(&process, &core::CoreProcess::stopped);
        process.start(config, environment_->dataDir());
        QTRY_COMPARE(lines.size(), 1);
        QElapsedTimer elapsed;
        elapsed.start();
        process.restart();
        QVERIFY2(elapsed.elapsed() < 200, "Restart blocked while stopping the child");
        QTRY_COMPARE(process.state(), core::CoreState::Stopping);
        QVERIFY(process.isRestartPending());
        bool tick = false;
        QTimer::singleShot(30, this, [&] { tick = true; });
        QTRY_VERIFY_WITH_TIMEOUT(tick, 500);
        QCOMPARE(lines.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(lines.size(), 2, 4500);
        process.stop();
        QVERIFY(!process.isRestartPending());
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QCOMPARE(stopped.size(), 1);
        process.stop();
        QCOMPARE(stopped.size(), 2);
#endif
    }
    // REGRESSION. The kill that ends a child which refuses SIGTERM was armed
    // with QTimer::singleShot(terminateWaitMs, child, ...). With no timer type
    // that call asks QTimer::defaultTypeFor(), which answers Qt::CoarseTimer
    // for any interval of 2 s or more - and a coarse timer may fire EARLY. The
    // published wait is 3000 ms, so the escalation ran ahead of the grace: the
    // recovery journey watched a stubborn child die 2962 ms into it, and a
    // standalone probe on
    // this machine saw the same call fire at 2851 ms. A child killed before its
    // grace expires was never given the grace.
    //
    // The case is deliberately driven at the SHIPPED wait rather than at a
    // value pushed in through setTimings(). Below 2 s the very same call picks
    // a precise timer, so the 400 ms the workflow used to compress this to made
    // the defect unreachable - the budget under test has to be one that reaches
    // the coarse path. The three seconds are the honest price.
    void aChildRefusingToTerminateKeepsItsWholePublishedGraceBeforeTheKill() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake core process; terminate() cannot be refused here");
#else
        const QString binary = environment_->filePath("refusing-core");
        // The marker is printed AFTER SIG_IGN is installed, and the case waits
        // for it: measuring against a child that has not yet armed its refusal
        // would time a polite exit and prove nothing about the escalation.
        testsupport::writeFile(binary, "#!/usr/bin/python3\nimport signal,sys,time\n"
            "if '-t' in sys.argv: sys.exit(0)\n"
            "signal.signal(signal.SIGTERM,signal.SIG_IGN)\n"
            "print('refusal-armed',flush=True)\ntime.sleep(30)\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        // Listens and never answers, so the child stays up while it is probed.
        QTcpServer controller;
        QVERIFY(controller.listen(QHostAddress::LocalHost));
        const QString config = environment_->filePath("config.yaml");
        testsupport::writeFile(config, "external-controller: 127.0.0.1:" +
            QByteArray::number(controller.serverPort()) + "\n");
        core::CoreProcess process;
        process.setBinaryPath(binary);
        const int published = process.timings().terminateWaitMs;
        QVERIFY2(published >= 2000, "a wait under 2 s is armed on a precise timer anyway, "
                                    "so this case would not cover the escalation that ships");
        QSignalSpy lines(&process, &core::CoreProcess::logLine);
        QSignalSpy stopped(&process, &core::CoreProcess::stopped);
        process.start(config, environment_->dataDir());
        QTRY_VERIFY(!lines.isEmpty());
        QVERIFY(lines.first().first().toString().contains("refusal-armed"));

        QElapsedTimer elapsed;
        elapsed.start();
        process.stop();
        QCOMPARE(process.state(), core::CoreState::Stopping);
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), core::CoreState::Stopped, published + 5000);
        const qint64 took = elapsed.elapsed();
        QCOMPARE(stopped.size(), 1);
        // The bound, in both directions: the child refused the polite request,
        // so it cannot have gone before its grace ran out, and the escalation
        // must still have ended it soon after.
        QVERIFY2(took >= published,
                 qPrintable(QStringLiteral("the child was killed %1 ms into its published %2 ms "
                                           "termination grace")
                                .arg(took)
                                .arg(published)));
        QVERIFY2(took < published + 5000,
                 qPrintable(QStringLiteral("the stop took %1 ms against a %2 ms published "
                                           "termination wait: the escalation is not bounded")
                                .arg(took)
                                .arg(published)));
#endif
    }

    // REGRESSION. An earlier wave replaced CoreProcess's default-constructed
    // platform::PrivilegedServiceClient with NullPrivilegedCoreService, whose
    // isSupported() is false. main.cpp passed no service, so service mode became
    // silently unavailable on macOS: setUseService(true) returned false and the
    // saved core/useService preference was discarded without a word. Nothing
    // covered it, and the suite stayed green. These two cases pin both halves of
    // the seam: an un-injected process must refuse, and an injected one must not.
    void anUninjectedProcessHasNoPrivilegedServiceAndSaysSo() {
        core::CoreProcess process;
        QVERIFY(!process.isServiceSupported());
        QVERIFY(!process.setUseService(true));
        QVERIFY(!process.isServiceMode());
    }

    void anInjectedAdapterMakesServiceModeAvailableWhereThePlatformSupportsIt() {
#ifndef Q_OS_MACOS
        QSKIP("privileged service mode is macOS-only");
#else
        platform::PrivilegedServiceClient client;
        core::PrivilegedServiceClientAdapter service(&client, QStringLiteral("/tmp/clash-qt-seam-probe.socket"));
        core::CoreProcess process(nullptr, &service);
        QVERIFY(process.isServiceSupported());
        QVERIFY(process.setUseService(true));
        QVERIFY(process.isServiceMode());
#endif
    }

    void privilegedServiceLifecycleUsesLeaseAndWaitsForStopAck() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake validator");
#else
        QTcpServer controller;
        QVERIFY(controller.listen(QHostAddress::LocalHost));
        connect(&controller, &QTcpServer::newConnection, &controller, [&] {
            auto *socket = controller.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                const QByteArray body = "{\"version\":\"service-test\"}";
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        const QString socketPath = environment_->socketPath(QStringLiteral("mock-service"));
        QLocalServer helper;
        QVERIFY(helper.listen(socketPath));
        QStringList commands;
        QJsonObject receivedConfig;
        QLocalSocket *peer = nullptr;
        QJsonObject pendingStop;
        bool stallLogs = false;
        auto respond = [](QLocalSocket *socket, const QJsonObject &response) {
            const QByteArray json = QJsonDocument(response).toJson(QJsonDocument::Compact);
            QByteArray frame(4, Qt::Uninitialized);
            qToBigEndian<quint32>(json.size(), frame.data());
            socket->write(frame + json);
        };
        connect(&helper, &QLocalServer::newConnection, &helper, [&] {
            peer = helper.nextPendingConnection();
            auto input = std::make_shared<QByteArray>();
            connect(peer, &QLocalSocket::readyRead, peer, [&, input] {
                input->append(peer->readAll());
                while (input->size() >= 4) {
                    const auto length = qFromBigEndian<quint32>(input->constData());
                    if (input->size() < length + 4) return;
                    const auto request = QJsonDocument::fromJson(input->mid(4, length)).object();
                    input->remove(0, length + 4);
                    QCOMPARE(request.value("protocol").toInt(), 1);
                    const QString command = request.value("command").toString();
                    commands.append(command);
                    QJsonObject response{{"protocol", 1}, {"id", request.value("id")}, {"ok", true}};
                    if (command == "start") {
                        receivedConfig = request.value("config").toObject();
                        response.insert("endpoint", QJsonObject{{"host", "127.0.0.1"},
                            {"port", controller.serverPort()}, {"secret", "0123456789abcdef0123456789abcdef"}});
                    } else if (command == "stop") { pendingStop = response; continue; }
                    else if (command == "logs") {
                        if (stallLogs) continue;
                        response.insert("logs", "service-started\n");
                    } else if (command == "status") response.insert("state", "running");
                    respond(peer, response);
                }
            });
        });
        const QString binary = environment_->filePath("validator");
        testsupport::writeFile(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then exit 0; fi\nexit 99\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        const QString config = environment_->filePath("service-config.yaml");
        testsupport::writeFile(config, "proxies: []\nsecret: '0123456789abcdef0123456789abcdef'\nexternal-controller: 127.0.0.1:" +
            QByteArray::number(controller.serverPort()) + "\ncustom-string: '123'\n");
        // The stalled-logs case below expires this client's request on purpose;
        // every other request here is answered by hand, so none of them expire.
        // Local to this case: it replaces searching the client for its QTimer,
        // and nothing here then depends on wall-clock time.
        class ManualDeadline : public platform::RequestDeadline {
        public:
            void setExpiredHandler(std::function<void()> handler) override {
                expired_ = std::move(handler);
            }
            void start(std::chrono::milliseconds) override { pending_ = true; }
            void stop() override { pending_ = false; }
            bool expire() {
                if (!pending_ || !expired_) return false;
                pending_ = false;
                expired_();
                return true;
            }

        private:
            std::function<void()> expired_;
            bool pending_ = false;
        };
        ManualDeadline deadline;
        platform::PrivilegedServiceClient client(nullptr, socketPath, &deadline);
        // The composition root - here, the test - adapts the concrete platform
        // client onto the seam the component owns, which is why CoreProcess's
        // constructor no longer names a platform type at all.
        core::PrivilegedServiceClientAdapter service(&client, socketPath);
        core::CoreProcess process(nullptr, &service);
        process.setBinaryPath(binary);
        QVERIFY(process.setUseService(true));
        QVERIFY(process.isServiceMode());
        QVERIFY(!process.usesPrivilegedService());
        QSignalSpy ready(&process, &core::CoreProcess::ready);
        QSignalSpy stopped(&process, &core::CoreProcess::stopped);
        QSignalSpy failures(&process, &core::CoreProcess::failed);
        process.start(config, environment_->dataDir());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QCOMPARE(ready.size(), 1);
        QVERIFY(process.usesPrivilegedService());
        QVERIFY(!process.setUseService(false));
        QCOMPARE(receivedConfig.value("custom-string").toString(), QString("123"));
        // The last core output must survive the status-to-failure transition.
        emit client.logsReceived("fatal: YAML unknown escape in configuration\n");
        emit client.statusReceived(QJsonObject{{"state", "stopped"}});
        QTRY_VERIFY(!pendingStop.isEmpty());
        respond(peer, pendingStop);
        pendingStop = {};
        QTRY_COMPARE(process.state(), core::CoreState::Failed);
        QCOMPARE(failures.size(), 1);
        QVERIFY(failures.first().first().toString().contains("YAML unknown escape"));
        failures.clear();
        process.start(config, environment_->dataDir());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        // A polling timeout must retain its cause, release the lease locally,
        // and allow a fresh start after the helper becomes responsive again.
        stallLogs = true;
        const auto previousCommands = commands.size();
        client.requestLogs();
        QTRY_VERIFY(commands.size() > previousCommands);
        QTRY_COMPARE(commands.last(), QString("logs"));
        QVERIFY(client.isBusy());
        QVERIFY(deadline.expire());
        QTRY_COMPARE(process.state(), core::CoreState::Failed);
        QCOMPARE(failures.size(), 1);
        QVERIFY(failures.first().first().toString().contains("timed out: logs"));
        QVERIFY(!process.usesPrivilegedService());
        QVERIFY(!client.isConnected());
        stallLogs = false;
        failures.clear();
        process.start(config, environment_->dataDir());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QVERIFY(process.usesPrivilegedService());
        QVERIFY(client.connectionError().isEmpty());
        QVERIFY(failures.isEmpty());
        process.stop();
        QTRY_VERIFY(!pendingStop.isEmpty());
        QCOMPARE(process.state(), core::CoreState::Stopping);
        QVERIFY(stopped.isEmpty());
        respond(peer, pendingStop);
        pendingStop = {};
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QCOMPARE(stopped.size(), 1);
        QVERIFY(!process.usesPrivilegedService());
        QVERIFY(failures.isEmpty());
        process.start(config, environment_->dataDir());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QSignalSpy terminal(&process, &core::CoreProcess::stopFinished);
        process.stop();
        QTRY_VERIFY(!pendingStop.isEmpty());
        peer->abort();
        QTRY_COMPARE(process.state(), core::CoreState::Failed);
        QTRY_COMPARE(terminal.size(), 1);
        QVERIFY(!terminal.first()[0].toBool());
        QVERIFY(!terminal.first()[1].toString().isEmpty());
        QVERIFY(!process.usesPrivilegedService());
        QVERIFY(!failures.isEmpty());
        QVERIFY(process.setUseService(false));
#endif
    }
    void validationIsResponsiveAndCancelable() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake core process");
#else
        const QString binary = environment_->filePath("slow-core");
        testsupport::writeFile(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then exec sleep 30; fi\nexit 0\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        core::CoreProcess process;
        process.setBinaryPath(binary);
        QElapsedTimer elapsed;
        elapsed.start();
        process.start(environment_->filePath("unused.yaml"), environment_->dataDir());
        QVERIFY2(elapsed.elapsed() < 500, "start() blocked while validating");
        bool tick = false;
        QTimer::singleShot(10, this, [&] { tick = true; });
        QTRY_VERIFY_WITH_TIMEOUT(tick, 500);
        QCOMPARE(process.state(), core::CoreState::Starting);
        QSignalSpy failures(&process, &core::CoreProcess::failed);
        process.stop();
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QTest::qWait(100);
        QCOMPARE(process.state(), core::CoreState::Stopped);
        QVERIFY(failures.isEmpty());
        // A new request cancels the older validator rather than waiting for it.
        process.start(environment_->filePath("unused.yaml"), environment_->dataDir());
        const QString invalid = environment_->filePath("invalid-core");
        testsupport::writeFile(invalid, "#!/bin/sh\necho invalid-new-request\nexit 1\n");
        QVERIFY(QFile::setPermissions(invalid, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        process.setBinaryPath(invalid);
        process.start(environment_->filePath("unused.yaml"), environment_->dataDir());
        QTRY_COMPARE_WITH_TIMEOUT(process.state(), core::CoreState::Failed, 1000);
        QCOMPARE(failures.size(), 1);
        QVERIFY(failures.first().first().toString().contains("invalid-new-request"));
#endif
    }
    void hangingProbeCanStopAndRestart() {
#ifdef Q_OS_WIN
        QSKIP("Uses a POSIX fake core process");
#else
        const QString binary = environment_->filePath("fake-core");
        testsupport::writeFile(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then exit 0; fi\nexec sleep 30\n");
        QVERIFY(QFile::setPermissions(binary, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QTcpServer hanging;
        QVERIFY(hanging.listen(QHostAddress::LocalHost));
        QSignalSpy probes(&hanging, &QTcpServer::newConnection);
        const QString config = environment_->filePath("config.yaml");
        testsupport::writeFile(config, "external-controller: 127.0.0.1:" + QByteArray::number(hanging.serverPort()) + "\n");
        core::CoreProcess process;
        process.setBinaryPath(binary);
        QSignalSpy ready(&process, &core::CoreProcess::ready);
        process.start(config, environment_->dataDir());
        QTRY_VERIFY(hanging.hasPendingConnections());
        QTRY_VERIFY_WITH_TIMEOUT(probes.size() >= 2, 4000);
        process.stop();
        QTRY_COMPARE(process.state(), core::CoreState::Stopped);
        QTcpServer healthy;
        QVERIFY(healthy.listen(QHostAddress::LocalHost));
        connect(&healthy, &QTcpServer::newConnection, &healthy, [&] {
            QTcpSocket *socket = healthy.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                socket->readAll();
                const QByteArray body = "{\"version\":\"test\"}";
                socket->write("HTTP/1.1 200 OK\r\nContent-Length: " + QByteArray::number(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        testsupport::writeFile(config, "external-controller: 127.0.0.1:" + QByteArray::number(healthy.serverPort()) + "\n");
        process.start(config, environment_->dataDir());
        QTRY_COMPARE(process.state(), core::CoreState::Running);
        QCOMPARE(ready.size(), 1);
        // A bad edit must not stop the healthy child process.
        testsupport::writeFile(binary, "#!/bin/sh\nif [ \"$1\" = '-t' ]; then echo invalid-config; exit 1; fi\nexec sleep 30\n");
        QSignalSpy failures(&process, &core::CoreProcess::failed);
        process.restart();
        QCOMPARE(process.state(), core::CoreState::Running);
        QTRY_COMPARE(failures.size(), 1);
        QCOMPARE(process.state(), core::CoreState::Running);
        QVERIFY(failures.first().first().toString().contains("invalid-config"));
        process.stop();
#endif
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(CoreProcessTest)
#include "core_process_test.moc"
