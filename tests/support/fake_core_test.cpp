#include <QtTest>

#include <QProcess>
#include <QTemporaryDir>

#include "support/fake_core.h"

using testsupport::FakeCore;
using testsupport::TerminationContract;

class FakeCoreTest : public QObject {
    Q_OBJECT
    QTemporaryDir directory_;

    FakeCore make(const QString &name) {
        return FakeCore(directory_.path() + QStringLiteral("/") + name, name);
    }

    // Production passes exactly these arguments (core_process.cpp:281, :393).
    static QStringList runArguments(const FakeCore &core) {
        return {QStringLiteral("-d"), core.directory(),
                QStringLiteral("-f"), core.directory() + QStringLiteral("/config.yaml")};
    }
    static QStringList validateArguments(const FakeCore &core) {
        return QStringList{QStringLiteral("-t")} + runArguments(core);
    }

private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        QVERIFY2(QFileInfo::exists(FakeCore::fixtureBinaryPath()),
                 qPrintable(QStringLiteral("No fake-core fixture at ") + FakeCore::fixtureBinaryPath()));
    }

    void validationSucceedsAndRecordsItsArguments() {
        FakeCore core = make(QStringLiteral("ok-validator"));
        QVERIFY2(core.isValid(), qPrintable(core.errorString()));
        QVERIFY(core.validationSucceeds(QStringLiteral("configuration accepted")).commit());

        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(core.binaryPath(), validateArguments(core));
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QCOMPARE(QString::fromUtf8(process.readAll()).trimmed(), QStringLiteral("configuration accepted"));

        QCOMPARE(core.invocationCount(), 1);
        QVERIFY2(core.invocations().first().startsWith(QStringLiteral("-t -d ")),
                 qPrintable(core.invocations().first()));
    }

    void validationFailureReportsItsMessageAndExitCode() {
        FakeCore core = make(QStringLiteral("bad-validator"));
        QVERIFY(core.isValid());
        QVERIFY(core.validationFails(QStringLiteral("rule 3: unknown proxy"), 99).commit());

        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(core.binaryPath(), validateArguments(core));
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 99);
        QCOMPARE(QString::fromUtf8(process.readAll()).trimmed(), QStringLiteral("rule 3: unknown proxy"));
    }

    void validationCanBeHeldOpenAndThenReleased() {
        FakeCore core = make(QStringLiteral("slow-validator"));
        QVERIFY(core.isValid());
        QVERIFY(core.validationSucceeds().validationWaitsFor(QStringLiteral("finish-validation")).commit());

        QProcess process;
        process.start(core.binaryPath(), validateArguments(core));
        QVERIFY(process.waitForStarted(5000));
        // Observable pending state, with no sleep deciding the outcome.
        QVERIFY(!process.waitForFinished(300));
        QCOMPARE(process.state(), QProcess::Running);

        QVERIFY(core.release(QStringLiteral("finish-validation")));
        QVERIFY2(process.waitForFinished(5000), "Released validation never finished");
        QCOMPARE(process.exitCode(), 0);
    }

    void readinessLinesAndMarkersAreOrdered() {
        FakeCore core = make(QStringLiteral("ready-core"));
        QVERIFY(core.isValid());
        QVERIFY(core.printsLine(QStringLiteral("time=\"start\" level=info msg=\"loading\""))
                    .touches(QStringLiteral("started-once"))
                    .waitsFor(QStringLiteral("allow-ready"))
                    .printsLine(QStringLiteral("time=\"ready\" level=info msg=\"listening\""))
                    .exitsWith(0)
                    .commit());

        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(core.binaryPath(), runArguments(core));
        QVERIFY(process.waitForStarted(5000));
        QVERIFY(process.waitForReadyRead(5000));
        QByteArray output = process.readAll();
        QVERIFY2(output.contains("loading"), output.constData());
        QVERIFY(!output.contains("listening"));
        QTRY_VERIFY_WITH_TIMEOUT(core.reached(QStringLiteral("started-once")), 5000);
        QCOMPARE(process.state(), QProcess::Running);

        QVERIFY(core.release(QStringLiteral("allow-ready")));
        QVERIFY(process.waitForFinished(5000));
        output += process.readAll();
        QVERIFY2(output.contains("listening"), output.constData());
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
    }

    void controlledExitCodeIsReported() {
        FakeCore core = make(QStringLiteral("exiting-core"));
        QVERIFY(core.isValid());
        QVERIFY(core.printsLine(QStringLiteral("fatal: address in use")).exitsWith(7).commit());

        QProcess process;
        process.start(core.binaryPath(), runArguments(core));
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 7);
    }

    void crashMatchesTheDeclaredPlatformContract() {
        const TerminationContract contract = FakeCore::terminationContract();
        FakeCore core = make(QStringLiteral("crashing-core"));
        QVERIFY(core.isValid());
        QVERIFY(core.printsLine(QStringLiteral("about to fault")).crashes().commit());

        QProcess process;
        process.start(core.binaryPath(), runArguments(core));
        QVERIFY(process.waitForFinished(5000));
        if (contract.crashReportsCrashExit) {
            QCOMPARE(process.exitStatus(), QProcess::CrashExit);
        } else {
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QCOMPARE(process.exitCode(), contract.crashExitCode);
        }
    }

    // Neither of the next two cases skips anywhere: each asserts the documented
    // behaviour of the host OS, so a platform difference is a visible assertion
    // rather than an invisible QSKIP.
    void terminateStopsAnOrdinaryCoreWhereTerminateIsCooperative() {
        const TerminationContract contract = FakeCore::terminationContract();
        FakeCore core = make(QStringLiteral("polite-core"));
        QVERIFY(core.isValid());
        QVERIFY(core.printsLine(QStringLiteral("running")).runsForever().commit());

        QProcess process;
        process.start(core.binaryPath(), runArguments(core));
        QVERIFY(process.waitForStarted(5000));
        QVERIFY(process.waitForReadyRead(5000));
        process.terminate();
        QCOMPARE(process.waitForFinished(2000), contract.terminateIsCooperative);
        if (!contract.terminateIsCooperative) {
            process.kill();
            QVERIFY(process.waitForFinished(5000));
        }
    }

    void killAlwaysStopsAStubbornCore() {
        FakeCore core = make(QStringLiteral("stubborn-core"));
        QVERIFY(core.isValid());
        QVERIFY(core.ignoresTerminate().printsLine(QStringLiteral("running")).runsForever().commit());

        QProcess process;
        process.start(core.binaryPath(), runArguments(core));
        QVERIFY(process.waitForStarted(5000));
        QVERIFY(process.waitForReadyRead(5000));

        process.terminate();
        QVERIFY2(!process.waitForFinished(1000), "A stubborn core must survive terminate() on every OS");
        QCOMPARE(process.state(), QProcess::Running);

        process.kill();
        QVERIFY2(process.waitForFinished(5000), "kill() must always win");
        QCOMPARE(process.exitStatus(), QProcess::CrashExit);
    }

    void restartUsesTheSameBinaryAndRecordsBothInvocations() {
        FakeCore core = make(QStringLiteral("restarting-core"));
        QVERIFY(core.isValid());
        QVERIFY(core.validationSucceeds().printsLine(QStringLiteral("up")).exitsWith(0).commit());

        for (int run = 0; run < 2; ++run) {
            QProcess validation;
            validation.start(core.binaryPath(), validateArguments(core));
            QVERIFY(validation.waitForFinished(5000));
            QProcess process;
            process.start(core.binaryPath(), runArguments(core));
            QVERIFY(process.waitForFinished(5000));
        }
        QCOMPARE(core.invocationCount(), 4);
        QVERIFY(core.invocations().at(0).startsWith(QStringLiteral("-t ")));
        QVERIFY(!core.invocations().at(1).startsWith(QStringLiteral("-t ")));
    }
};

QTEST_GUILESS_MAIN(FakeCoreTest)
#include "fake_core_test.moc"
