// G1 engine resolution.
//
// The defect this guards: CoreProcess::discoverBinary() used to fall through
// from the staged path to QStandardPaths::findExecutable("mihomo") and then to
// a hardcoded "/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo". On a
// machine with Clash Verge installed - the machine this was found on - an
// application with no staged engine silently supervised Clash Verge's binary
// while reporting its own provenance.
//
// What must hold now: the managed path resolves the engine this project staged
// or nothing; anything else is an explicit, separately labelled choice; and
// whatever is resolved is reported.

#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/mihomo/process/engine_discovery.h"
#include "support/scoped_environment.h"

class EngineDiscoveryTest : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("eng"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
        environment_->unsetEnvironment("CLASH_QT_CORE_BINARY");
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
        // provenance: that is precisely the claim G1 forbids implying.
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
