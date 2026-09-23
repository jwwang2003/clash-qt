// Which ENGINE this process supervises, and which CONTROLLER it attaches to.
// One file, because they are the same defect twice.
//
// THE ENGINE HALF. CoreProcess::discoverBinary() used to fall through from the
// staged path to QStandardPaths::findExecutable("mihomo") and then to a
// hardcoded "/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo". On a
// machine with Clash Verge installed - the machine this was found on - an
// application with no staged engine silently supervised Clash Verge's binary
// while reporting its own provenance.
//
// What must hold now: the managed path resolves the engine this project staged
// or nothing; anything else is an explicit, separately labelled choice; and
// whatever is resolved is reported.
//
// THE CONTROLLER HALF, and why it is the same defect. core::discoverEndpoint()
// ended in `return Endpoint{}` - and core::Endpoint's default is the
// conventional 127.0.0.1:9090, whose isValid() is true. So a process that found
// no controller anywhere reported that it had found one, at the address another
// running client almost always occupies. Before that it read Clash Verge Rev's
// own config.yaml. An isolated launch - `--data-dir`, every workflow suite,
// every smoke child - therefore attached to, polled and could have driven an
// engine belonging to a different installation, and nothing in the tree could
// see it happen.
//
// What must hold now: a selected data directory attaches to NOTHING it was not
// given; an explicit CLASH_QT_CONTROLLER is the whole answer, including when it
// is wrong; and an ordinary launch keeps the legacy convenience unchanged.
//
// ISOLATION OF THIS SUITE. Every case runs inside a ScopedEnvironment, so
// CLASH_QT_DATA_DIR is always set while it runs and the isolated rule is the
// one under test. The legacy branch is driven through discoverEndpoint(inputs)
// with a FIXTURE config file this suite wrote: no case reads, or needs to read,
// another application's configuration, and none of them names a real
// controller.

#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>

#include "core/mihomo/controller_discovery.h"
#include "core/mihomo/mihomo_client.h"
#include "core/mihomo/process/engine_discovery.h"
#include "support/loopback_server.h"
#include "support/scoped_environment.h"

class EngineDiscoveryTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("eng"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
        environment_->unsetEnvironment("CLASH_QT_CORE_BINARY");
        // A developer who exports a controller into their shell must not be
        // able to change what any case here observes. Each controller case puts
        // back exactly what it means to test.
        environment_->unsetEnvironment("CLASH_QT_CONTROLLER");
        environment_->unsetEnvironment("CLASH_QT_SECRET");
    }
    void cleanup() { environment_.reset(); }

    void theManagedPathNeverResolvesPathOrAnotherClashInstallation() {
        // Put an executable called "mihomo" on PATH, which is exactly what the
        // old fallback would have picked up.
        const QString decoy = makeExecutable(QStringLiteral("mihomo"));
        environment_->setEnvironment("PATH", environment_->rootPath().toUtf8() + ":/usr/bin:/bin");

        const core::EngineResolution resolution = core::resolveManagedEngine();
        QVERIFY2(!resolution.isResolved(),
                 "the managed path resolved an engine this application did not stage");
        QVERIFY2(resolution.path != decoy, "the managed path used an executable from PATH");
        QVERIFY(!resolution.problem.isEmpty());
        QVERIFY2(resolution.problem.contains(QStringLiteral("make core")),
                 "the failure message is not actionable");

        // It is still OFFERED - separately, and labelled as what it is.
        const QVector<core::EngineCandidate> offered = core::externalEngineCandidates();
        bool sawTheDecoy = false;
        for (const core::EngineCandidate &candidate : offered) {
            if (candidate.path != decoy) continue;
            sawTheDecoy = true;
            QCOMPARE(candidate.source, core::EngineSource::SearchPath);
            QVERIFY(!candidate.label.isEmpty());
            QVERIFY2(candidate.label != core::engineSourceLabel(core::EngineSource::Staged),
                     "a PATH engine was labelled as the staged one");
        }
        QVERIFY2(sawTheDecoy, "the explicit-choice list did not offer the PATH engine at all");
    }

    void aLocalBuildIsAManagedEngineAndIsLabelledAsOne() {
        const QString engine = makeExecutable(QStringLiteral("built-mihomo"));
        environment_->setEnvironment("CLASH_QT_CORE_BINARY", engine.toUtf8());

        const core::EngineResolution resolution = core::resolveManagedEngine();
        QVERIFY(resolution.isResolved());
        QCOMPARE(resolution.path, engine);
        QCOMPARE(resolution.source, core::EngineSource::LocalBuild);
        QVERIFY(resolution.label.contains(QStringLiteral("CLASH_QT_CORE_BINARY")));
        QVERIFY2(resolution.problem.isEmpty(), "a resolved engine carried a problem");
    }

    void anUnusableLocalBuildIsNotResolved() {
        environment_->setEnvironment("CLASH_QT_CORE_BINARY",
                                     environment_->filePath(QStringLiteral("absent")).toUtf8());
        QVERIFY(!core::resolveManagedEngine().isResolved());

        // Present but not executable is equally not an engine.
        const QString data = environment_->filePath(QStringLiteral("not-executable"));
        QFile file(data);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\n");
        file.close();
        environment_->setEnvironment("CLASH_QT_CORE_BINARY", data.toUtf8());
        QVERIFY(!core::resolveManagedEngine().isResolved());
    }

    void provenanceIsReportedAndIsTiedToTheArtifactItDescribes() {
        const QString engine = makeExecutable(QStringLiteral("provenanced"));
        writeProvenance(engine, sha256Of(engine));

        const QString reported = core::engineProvenance(engine);
        QVERIFY(reported.contains(QStringLiteral("v1.19.31")));
        QVERIFY(reported.contains(QStringLiteral("clean")));
        QVERIFY(reported.contains(sha256Of(engine).left(12)));

        // A manifest that describes a DIFFERENT artifact must not lend it its
        // provenance: reporting a provenance the binary does not have is exactly
        // the claim this project may not imply.
        writeProvenance(engine, QString(64, QLatin1Char('a')));
        const QString mismatched = core::engineProvenance(engine);
        QVERIFY2(!mismatched.contains(QStringLiteral("v1.19.31")),
                 "a stale manifest lent its provenance to a different binary");
        QVERIFY(mismatched.contains(QStringLiteral("does not match")));
    }

    void anEngineWithoutAManifestSaysSoRatherThanStayingSilent() {
        const QString engine = makeExecutable(QStringLiteral("unprovenanced"));
        const core::EngineResolution resolution = core::describeChosenEngine(engine);
        QVERIFY(resolution.isResolved());
        QCOMPARE(resolution.source, core::EngineSource::UserChosen);
        QCOMPARE(resolution.provenance, QStringLiteral("no recorded provenance"));
    }

    void anExplicitChoiceIsReportedAsAChoice() {
        const QString engine = makeExecutable(QStringLiteral("chosen"));
        const core::EngineResolution resolution = core::describeChosenEngine(engine);
        QCOMPARE(resolution.source, core::EngineSource::UserChosen);
        QVERIFY(!resolution.label.isEmpty());
        QVERIFY2(resolution.label != core::engineSourceLabel(core::EngineSource::Staged),
                 "an engine the user picked was reported as the staged one");

        const core::EngineResolution missing =
            core::describeChosenEngine(environment_->filePath(QStringLiteral("gone")));
        QVERIFY(!missing.isResolved());
        QVERIFY(missing.problem.contains(QStringLiteral("not an executable file")));
    }

    // ------------------------------------------------ controller resolution
    // >>> BEGIN MIRRORED CONTROLLER CASES
    // Everything between these markers is compiled verbatim in the external
    // snapshot under /tmp/clash-qt-p4.w0Y8Uo/isolation, which is where the
    // inversion proofs for it were taken. Keep them mechanically copyable.

    void anIsolatedProcessWithNoExplicitControllerAttachesToNothing() {
        // The scope has already exported CLASH_QT_DATA_DIR; that IS the
        // isolation signal the composition root gives discovery.
        QVERIFY(qEnvironmentVariableIsSet("CLASH_QT_DATA_DIR"));
        QVERIFY(!qEnvironmentVariableIsSet("CLASH_QT_CONTROLLER"));

        const core::Endpoint discovered = core::discoverEndpoint();
        QVERIFY2(!discovered.isValid(),
                 qPrintable(QStringLiteral(
                                "an isolated launch resolved a controller at %1:%2. src/main.cpp "
                                "attaches to whatever this returns, so that address - which on a "
                                "developer's machine is another client's live core - would have "
                                "been polled and could have been driven.")
                                .arg(discovered.host)
                                .arg(discovered.port)));
        QVERIFY(discovered.host.isEmpty());
        QCOMPARE(discovered.port, quint16(0));
        QVERIFY(discovered.secret.isEmpty());

        // Not merely "invalid": not the conventional default either. A
        // default-constructed core::Endpoint IS 127.0.0.1:9090 and IS valid,
        // and returning it is exactly the defect this case exists for.
        const core::Endpoint conventional;
        QVERIFY2(conventional.isValid(), "core::Endpoint's default stopped being the 9090 guess, "
                                         "so this case is no longer testing what it says");
        QVERIFY(discovered.port != conventional.port);
        QCOMPARE(discovered.host.isEmpty(), true);
        QCOMPARE(core::noControllerEndpoint().port, discovered.port);

        // The other installation's path is not merely unread: for an isolated
        // process it is never even assembled, so no later caller can read it by
        // forgetting to ask about isolation first.
        const core::ControllerDiscoveryInputs inputs = core::processDiscoveryInputs();
        QVERIFY(inputs.isolated);
        QVERIFY2(inputs.foreignConfigPath.isEmpty(),
                 qPrintable(QStringLiteral("an isolated process still names %1 as a place to look")
                                .arg(inputs.foreignConfigPath)));
        QVERIFY(!inputs.explicitControllerSet);
    }

    void anExplicitControllerIsAuthoritativeAndCarriesItsSecret() {
        environment_->setEnvironment("CLASH_QT_CONTROLLER", "127.0.0.1:29411");
        environment_->setEnvironment("CLASH_QT_SECRET", "fixture-secret");

        const core::Endpoint discovered = core::discoverEndpoint();
        QVERIFY2(discovered.isValid(), "an explicit controller was not resolved at all");
        QCOMPARE(discovered.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(discovered.port, quint16(29411));
        QCOMPARE(discovered.secret, QStringLiteral("fixture-secret"));
        QCOMPARE(discovered.httpBase(), QStringLiteral("http://127.0.0.1:29411"));
    }

    void aBadExplicitControllerResolvesToNothingAndNeverFallsBack_data() {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("isolated");
        // Each of these used to fall THROUGH to the next source, which is the
        // behaviour the architecture decision forbids: a host that named a
        // controller and got the name wrong asked for that controller.
        QTest::newRow("no port, isolated") << QStringLiteral("127.0.0.1") << true;
        QTest::newRow("no port, ordinary launch") << QStringLiteral("127.0.0.1") << false;
        QTest::newRow("port zero") << QStringLiteral("127.0.0.1:0") << false;
        QTest::newRow("port is not a number") << QStringLiteral("127.0.0.1:nine") << false;
        QTest::newRow("port out of range") << QStringLiteral("127.0.0.1:70000") << false;
        QTest::newRow("empty but set") << QString() << false;
        QTest::newRow("whitespace only") << QStringLiteral("   ") << false;
        QTest::newRow("bracketed v6 without a port") << QStringLiteral("[::1]") << false;
        QTest::newRow("bracket never closed") << QStringLiteral("[::1:9090") << false;
    }

    void aBadExplicitControllerResolvesToNothingAndNeverFallsBack() {
        QFETCH(QString, value);
        QFETCH(bool, isolated);

        // A LOCAL DECOY, never a real installation. It stands where another
        // client's config.yaml would stand, and the claim is that a bad
        // explicit value does not reach it.
        const QString decoy = writeControllerConfig(QStringLiteral("decoy.yaml"),
                                                    QStringLiteral("127.0.0.1:29412"),
                                                    QStringLiteral("decoy-secret"));
        QVERIFY2(core::endpointFromConfigFile(decoy).has_value(),
                 "the decoy config is not parseable, so reaching it would not be visible");

        core::ControllerDiscoveryInputs inputs;
        inputs.explicitControllerSet = true;
        inputs.explicitController = value;
        inputs.explicitSecret = QStringLiteral("ignored");
        inputs.isolated = isolated;
        inputs.foreignConfigPath = decoy;

        const core::Endpoint discovered = core::discoverEndpoint(inputs);
        QVERIFY2(!discovered.isValid(),
                 qPrintable(QStringLiteral("a malformed CLASH_QT_CONTROLLER (%1) resolved %2:%3")
                                .arg(value, discovered.host)
                                .arg(discovered.port)));
        QVERIFY2(discovered.port != quint16(29412),
                 "a malformed explicit controller fell back to another installation's config");
        QVERIFY2(discovered.port != core::Endpoint{}.port,
                 "a malformed explicit controller fell back to the conventional 9090 guess");
        QVERIFY(discovered.secret.isEmpty());
    }

    void anExplicitControllerAcceptsTheAddressFormsMihomoWrites_data() {
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("host");
        QTest::addColumn<quint16>("port");
        QTest::addColumn<QString>("url");
        QTest::newRow("v4") << QStringLiteral("127.0.0.1:29413") << QStringLiteral("127.0.0.1")
                            << quint16(29413) << QStringLiteral("http://127.0.0.1:29413");
        // Listening everywhere is still dialled locally.
        QTest::newRow("v4 wildcard") << QStringLiteral("0.0.0.0:29414") << QStringLiteral("127.0.0.1")
                                     << quint16(29414) << QStringLiteral("http://127.0.0.1:29414");
        QTest::newRow("v6 loopback") << QStringLiteral("[::1]:29415") << QStringLiteral("::1")
                                     << quint16(29415) << QStringLiteral("http://[::1]:29415");
        QTest::newRow("v6 wildcard") << QStringLiteral("[::]:29416") << QStringLiteral("::1")
                                     << quint16(29416) << QStringLiteral("http://[::1]:29416");
        QTest::newRow("v6 unbracketed") << QStringLiteral("::1:29417") << QStringLiteral("::1")
                                        << quint16(29417) << QStringLiteral("http://[::1]:29417");
        QTest::newRow("named host") << QStringLiteral("localhost:29418")
                                    << QStringLiteral("localhost") << quint16(29418)
                                    << QStringLiteral("http://localhost:29418");
    }

    void anExplicitControllerAcceptsTheAddressFormsMihomoWrites() {
        QFETCH(QString, value);
        QFETCH(QString, host);
        QFETCH(quint16, port);
        QFETCH(QString, url);

        environment_->setEnvironment("CLASH_QT_CONTROLLER", value.toUtf8());
        environment_->setEnvironment("CLASH_QT_SECRET", "v6-secret");
        const core::Endpoint discovered = core::discoverEndpoint();
        QVERIFY2(discovered.isValid(), qPrintable(QStringLiteral("%1 did not resolve").arg(value)));
        QCOMPARE(discovered.host, host);
        QCOMPARE(discovered.port, port);
        QCOMPARE(discovered.httpBase(), url);
        QCOMPARE(discovered.secret, QStringLiteral("v6-secret"));

        // The file parser and the environment parser agree, so a controller
        // written into a config is reached the same way it is exported.
        const QString config = writeControllerConfig(QStringLiteral("agreeing.yaml"), value,
                                                     QStringLiteral("file-secret"));
        const auto fromFile = core::endpointFromConfigFile(config);
        QVERIFY(fromFile.has_value());
        QCOMPARE(fromFile->host, host);
        QCOMPARE(fromFile->port, port);
        QCOMPARE(fromFile->secret, QStringLiteral("file-secret"));
    }

    // The legacy convenience, unchanged, driven through a FIXTURE config. An
    // ordinary launch is the only caller that is still allowed to look at a
    // neighbouring installation, and removing that would break the case this
    // project shipped it for: a user who already runs another client and
    // expects clash-qt to find the core that is already up.
    void anOrdinaryLaunchStillFindsTheNeighbouringInstallation() {
        const QString config = writeControllerConfig(QStringLiteral("neighbour.yaml"),
                                                     QStringLiteral("127.0.0.1:29419"),
                                                     QStringLiteral("neighbour-secret"));
        core::ControllerDiscoveryInputs inputs;
        inputs.isolated = false;
        inputs.explicitControllerSet = false;
        inputs.foreignConfigPath = config;

        const core::Endpoint discovered = core::discoverEndpoint(inputs);
        QVERIFY2(discovered.isValid(), "an ordinary launch stopped discovering a running core");
        QCOMPARE(discovered.host, QStringLiteral("127.0.0.1"));
        QCOMPARE(discovered.port, quint16(29419));
        QCOMPARE(discovered.secret, QStringLiteral("neighbour-secret"));

        // And when there is nothing there, the bare localhost default it has
        // always ended on. Only for an ordinary launch.
        inputs.foreignConfigPath = environment_->filePath(QStringLiteral("absent.yaml"));
        QVERIFY(!QFileInfo::exists(inputs.foreignConfigPath));
        const core::Endpoint fallback = core::discoverEndpoint(inputs);
        QCOMPARE(fallback.host, core::Endpoint{}.host);
        QCOMPARE(fallback.port, core::Endpoint{}.port);

        // The SAME inputs, isolated: no file read, no guess.
        inputs.isolated = true;
        inputs.foreignConfigPath = config;
        QVERIFY2(!core::discoverEndpoint(inputs).isValid(),
                 "an isolated launch read a neighbouring installation's configuration");

        // Where that neighbour would be is still known - the seed-copy path in
        // the composition root needs it - and this suite does not read it.
        QVERIFY(core::vergeConfigPath().contains(QStringLiteral("clash-verge")));
    }

    // --- the stream the composition root subscribes to before there is an
    // --- address to dial
    //
    // src/main.cpp calls bridge.openTrafficStream() unconditionally, because
    // the subscription must survive until a managed core is attached. With no
    // endpoint that used to dial endpoint_.wsBase() + "/traffic" - built from
    // an empty host and port 0, so "ws:///traffic" - and then retry it forever.

    void aClientWithNoAddressDoesNotDialAndKeepsItsSubscription() {
        core::MihomoClient client;
        QVERIFY2(!client.endpoint().isValid(),
                 "a fresh client is supposed to be attached to nothing");
        QSignalSpy errors(&client, &core::MihomoClient::errorOccurred);

        client.openTrafficStream();
        // Longer than the minimum reconnect interval, so a dial that failed
        // would have failed AND retried by now.
        QTest::qWait(1500);
        QVERIFY2(errors.isEmpty(),
                 qPrintable(QStringLiteral(
                                "a client with no address dialled anyway: %1. There is no "
                                "controller at ws:///traffic to be right about, and the retry "
                                "timer reports this once a second for the life of the process.")
                                .arg(firstError(errors))));

        // The subscription is still live. Nothing calls openTrafficStream()
        // again - src/main.cpp calls it exactly once, at startup - so if the
        // pointer had been dropped the traffic producer would be gone for the
        // whole session.
        testsupport::LoopbackServer controller;
        QVERIFY2(controller.listen(QStringLiteral("stream-secret")), "the fixture did not bind");
        scriptMinimalController(controller);
        core::Endpoint endpoint;
        endpoint.host = QStringLiteral("127.0.0.1");
        endpoint.port = controller.port();
        endpoint.secret = QStringLiteral("stream-secret");
        client.setEndpoint(endpoint);

        QTRY_VERIFY2(controller.streamHandshakes(QStringLiteral("/traffic")) >= 1,
                     qPrintable(QStringLiteral(
                                    "attaching to a real controller did not open the traffic "
                                    "stream the composition root had already subscribed to: %1")
                                    .arg(controller.redactedTranscript())));
    }

    // The control arm. Without it, "nothing was dialled" would pass on a
    // harness that cannot see a dial at all.
    void aClientWithAnAddressDialsItsStreamAtOnce() {
        testsupport::LoopbackServer controller;
        QVERIFY2(controller.listen(QStringLiteral("stream-secret")), "the fixture did not bind");
        scriptMinimalController(controller);

        core::MihomoClient client;
        core::Endpoint endpoint;
        endpoint.host = QStringLiteral("127.0.0.1");
        endpoint.port = controller.port();
        endpoint.secret = QStringLiteral("stream-secret");
        client.setEndpoint(endpoint);
        client.openTrafficStream();

        QTRY_VERIFY2(controller.streamHandshakes(QStringLiteral("/traffic")) >= 1,
                     qPrintable(controller.redactedTranscript()));
    }

    // >>> END MIRRORED CONTROLLER CASES

    void everySourceHasADistinctUserFacingLabel() {
        QStringList labels;
        for (core::EngineSource source :
             {core::EngineSource::Staged, core::EngineSource::LocalBuild,
              core::EngineSource::UserChosen, core::EngineSource::ExternalInstall,
              core::EngineSource::SearchPath, core::EngineSource::None}) {
            const QString label = core::engineSourceLabel(source);
            QVERIFY(!label.isEmpty());
            QVERIFY2(!labels.contains(label), qPrintable(QStringLiteral("duplicate label: %1").arg(label)));
            labels.append(label);
        }
    }

  private:
    // >>> BEGIN MIRRORED CONTROLLER HELPERS

    /// A mihomo-shaped config.yaml inside this test's own scope. Never a real
    /// installation's file, and never a real controller's address.
    QString writeControllerConfig(const QString &name, const QString &controller,
                                  const QString &secret) {
        const QString path = environment_->filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        file.write(QStringLiteral("external-controller: '%1'\nsecret: %2\nmode: rule\n")
                       .arg(controller, secret)
                       .toUtf8());
        file.close();
        return QFileInfo(path).absoluteFilePath();
    }

    /// Enough for a client to settle after setEndpoint(): refreshState() asks
    /// for all four of these, and an unrouted path would answer 404 and turn
    /// into error noise that says nothing about the stream.
    static void scriptMinimalController(testsupport::LoopbackServer &controller) {
        using Reply = testsupport::LoopbackServer::Reply;
        controller.route("GET", QStringLiteral("/version"),
                         Reply::json(R"({"version":"fixture-1.19.31"})"));
        controller.route("GET", QStringLiteral("/configs"),
                         Reply::json(R"({"mode":"rule","mixed-port":0,"tun":{"enable":false}})"));
        controller.route("GET", QStringLiteral("/proxies"), Reply::json(R"({"proxies":{}})"));
        controller.route("GET", QStringLiteral("/rules"), Reply::json(R"({"rules":[]})"));
        controller.expectStream(QStringLiteral("/traffic"));
    }

    static QString firstError(const QSignalSpy &errors) {
        if (errors.isEmpty()) return QStringLiteral("(none)");
        return errors.first().value(0).toString();
    }

    // >>> END MIRRORED CONTROLLER HELPERS

    QString makeExecutable(const QString &name) {
        const QString path = environment_->filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
        file.write("#!/bin/sh\nexit 0\n");
        file.close();
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        return QFileInfo(path).absoluteFilePath();
    }

    static QString sha256Of(const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return {};
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&file);
        return QString::fromLatin1(hash.result().toHex());
    }

    void writeProvenance(const QString &engine, const QString &sha) {
        QJsonObject manifest;
        manifest.insert(QStringLiteral("source_describe"), QStringLiteral("v1.19.31"));
        manifest.insert(QStringLiteral("source_commit"),
                        QStringLiteral("ab405bad5beeeac8b003bb01f60f134f6df54471"));
        manifest.insert(QStringLiteral("source_state"), QStringLiteral("clean"));
        manifest.insert(QStringLiteral("go_toolchain"), QStringLiteral("go1.26.5 darwin/arm64"));
        manifest.insert(QStringLiteral("artifact_sha256"), sha);
        QFile file(QFileInfo(engine).absolutePath() + QStringLiteral("/mihomo-provenance.json"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(manifest).toJson());
    }

    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(EngineDiscoveryTest)
#include "engine_discovery_test.moc"
