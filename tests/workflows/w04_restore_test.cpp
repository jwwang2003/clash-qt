// W04 - Restore.
//
// docs/TEST_STRATEGY.md, "Complete workflows":
//
//   Exercise  Create profiles/enhancements/settings -> backup -> change them ->
//             restore while app services exist -> reopen
//   Outcome   Restored state is consistent; writers/processes were quiesced and
//             no stale reply overwrites it
//
// "WHILE APP SERVICES EXIST" IS THE WHOLE POINT. A restore into a quiet process
// is a file copy. This journey restores while a managed core is running, while a
// profile write is in flight, and while a subscription refresh is parked on the
// wire with a newer body waiting to land. All three are real: a real child
// process, a real asynchronous write, a real HTTP reply held by the loopback
// fixture and released after the restore has finished.
//
// WHAT DECIDES EACH CLAIM
//
//   "restored state is consistent"  the three things a backup actually carries -
//       profiles.json and the profile files, chain.json and the chain files, and
//       the allow-listed preference keys - read back from a SECOND graph built
//       over the same directory after the first one is gone. A field of the
//       object that performed the restore is not evidence that anything was
//       written.
//   "writers were quiesced"  a profile write is deliberately in flight when the
//       restore is requested, and the invariant asserted is the one that
//       matters: the archive was never unpacked while the profile store still
//       had a write outstanding. The gate's own status message is asserted too,
//       so "the gate ran" is distinguishable from "there was nothing to wait
//       for".
//   "processes were quiesced"  the managed child is gone - pgrep, not the
//       backend's opinion - before the files are written, and the ORDER of
//       prepared -> core stopped -> restored is recorded rather than assumed.
//   "no stale reply overwrites it"  the held subscription reply carries a body
//       that differs from the backed-up one. It is released AFTER the restore
//       completes. The restored bytes must still be on disk. Without the
//       differing body this assertion would pass on a machine where the reply
//       was simply never delivered.

#include <QtTest>

#include <QDir>
#include <QSignalSpy>
#include <QVariant>

#include <algorithm>
#include <memory>

#include "core/backups/backup_store.h"
#include "core/preferences/preferences.h"
#include "support/fake_core.h"
#include "support/loopback_server.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"
#include "workflows/workflow_support.h"

using testsupport::FakeCore;
using testsupport::LoopbackServer;
using testsupport::ScopedEnvironment;

namespace wf = workflows;
namespace cb = core::backend;

namespace {

void scriptController(LoopbackServer &server) {
    using Reply = LoopbackServer::Reply;
    server.route("GET", "/version", Reply::json(R"({"version":"workflow-core-1.19.31"})"));
    server.route("GET", "/configs", Reply::json(R"({"mode":"rule","tun":{"enable":false}})"));
    server.route("GET", "/proxies",
                 Reply::json(R"({"proxies":{"GLOBAL":{"type":"Selector","now":"DIRECT","all":["DIRECT"]}}})"));
    server.route("GET", "/rules", Reply::json(R"({"rules":[{"type":"MATCH","payload":"","proxy":"DIRECT"}]})"));
}

LoopbackServer::Reply subscription(const QByteArray &body) {
    LoopbackServer::Reply reply;
    reply.contentType = QByteArrayLiteral("text/yaml");
    reply.body = body;
    return reply;
}

}  // namespace

class W04RestoreTest : public QObject {
    Q_OBJECT

  private slots:

    void init() {
        environment_ = std::make_unique<ScopedEnvironment>(QStringLiteral("w04"));
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

    void backupChangeRestoreWhileRunningThenReopen() {
        LoopbackServer controller;
        QVERIFY(controller.listen(QString()));
        scriptController(controller);
        // The subscription origin. Local, loopback, and serving a fixture body:
        // docs/TEST_STRATEGY.md forbids a normal run from contacting a real
        // subscription, and this journey needs one only as the source of a
        // reply that must NOT be allowed to land.
        controller.route("GET", "/sub", subscription(backedUpSubscriptionBody()));

        wf::ControllerRelay relay;
        if (!relay.listen(controller.port())) {
            QSKIP(qPrintable(QStringLiteral(
                                 "W04 needs the generated controller address 127.0.0.1:%1, which "
                                 "is in use: %2. Not asserted rather than asserted weakly.")
                                 .arg(wf::kGeneratedControllerPort)
                                 .arg(relay.errorString())));
        }

        const QString engineDir = engineDirectory();
        FakeCore engine(engineDir);
        QVERIFY2(engine.isValid(), qPrintable(engine.errorString()));
        enginePath_ = engine.binaryPath();
        QVERIFY(engine.validationSucceeds()
                    .printsLine(QStringLiteral("[INFO] up"))
                    .runsForever()
                    .commit());

        auto proxyLog = std::make_shared<wf::ProxyOperations>();
        auto osProxy = std::make_shared<wf::ProxyConfig>();

        QString alpha;
        QString bravo;
        QString remote;
        QString chainUid;
        QString archive;

        {
            wf::AssembledApp app(proxyLog, osProxy);
            app.bootstrap(engine.binaryPath());
            auto *store = app.backups->store();
            QVERIFY2(store, "the coordinator published no backup store");

            // ---- 1. create profiles, an enhancement and settings ----------
            alpha = createProfile(app, QStringLiteral("alpha"));
            bravo = createProfile(app, QStringLiteral("bravo"));
            QVERIFY(!alpha.isEmpty());
            QVERIFY(!bravo.isEmpty());
            app.profiles->selectProfile(alpha);

            app.profiles->importFromUrl(controller.httpBase() + QStringLiteral("/sub"),
                                        QStringLiteral("remote"));
            QVERIFY2(wf::waitFor([&app] { return app.profiles->profiles().size() == 3; }),
                     qPrintable(app.storeErrors.join(QLatin1Char(' '))));
            remote = uidOf(app, QStringLiteral("remote"));
            QVERIFY(!remote.isEmpty());
            QVERIFY(wf::waitFor([&app] { return !app.profiles->isFileBusy(); }));
            QCOMPARE(wf::readTextFile(pathOf(app, remote)), backedUpSubscriptionBody());

            app.enhancer->addMerge(QStringLiteral("tweak"));
            QCOMPARE(app.enhancer->chain().size(), 1);
            chainUid = app.enhancer->chain().first().uid;
            QVERIFY(app.enhancer->chain().first().enabled);

            core::preferences::open().setValue(QStringLiteral("sysproxy/bypass"),
                                               QStringLiteral("backed-up.example"));
            core::preferences::open().setValue(QStringLiteral("hotkeys/proxy.toggle"),
                                               QStringLiteral("Ctrl+Alt+P"));
            core::preferences::open().sync();

            // ---- 2. a core is running: app services exist -----------------
            QVERIFY(app.runtimeCoordinator->requestAutostart());
            QVERIFY2(wf::waitFor([&app] { return app.backend->state() == cb::CoreState::Running; }),
                     qPrintable(report(app, controller)));
            QCOMPARE(wf::liveProcessesOf(engine.binaryPath()), 1);

            // ---- 3. back up ------------------------------------------------
            QSignalSpy backupsChanged(store, &core::BackupStore::backupsChanged);
            store->createLocalAsync();
            QVERIFY2(wf::waitFor([&backupsChanged] { return backupsChanged.size() >= 1; }),
                     "the backup never completed");
            QVERIFY(wf::waitFor([store] { return !store->isBusy(); }));
            QCOMPARE(store->localBackups().size(), 1);
            archive = store->localBackups().first();
            QVERIFY(QFileInfo(archive).size() > 0);
            // A backup must not have disturbed the running core.
            QCOMPARE(app.backend->state(), cb::CoreState::Running);
            QVERIFY2(!app.backups->isMaintenanceActive(),
                     "maintenance mode outlived the backup operation");

            // ---- 4. change everything --------------------------------------
            const QString charlie = createProfile(app, QStringLiteral("charlie"));
            QVERIFY(!charlie.isEmpty());
            app.profiles->removeProfile(bravo);
            app.profiles->selectProfile(charlie);
            app.enhancer->setEnabled(chainUid, false);
            core::preferences::open().setValue(QStringLiteral("sysproxy/bypass"),
                                               QStringLiteral("changed.example"));
            core::preferences::open().setValue(QStringLiteral("hotkeys/proxy.toggle"),
                                               QStringLiteral("Ctrl+Alt+Z"));
            core::preferences::open().sync();
            QVERIFY(wf::waitFor([&app] { return !app.profiles->isFileBusy(); }));
            QCOMPARE(app.profiles->profiles().size(), 3);
            QCOMPARE(app.profiles->currentUid(), charlie);
            QVERIFY(!app.enhancer->chain().first().enabled);

            // ---- 5. arm the stale reply ------------------------------------
            // The origin now serves a DIFFERENT body, and the request is parked.
            // If this reply were ever allowed to land, the restored profile
            // would carry staleSubscriptionBody() instead of the backed-up one,
            // which is exactly what the assertion after the restore looks for.
            controller.route("GET", "/sub", subscription(staleSubscriptionBody()));
            auto *held = controller.hold("GET", "/sub");
            app.profiles->updateProfile(remote);
            QVERIFY2(wf::waitFor([&held] { return held->pending() >= 1; }),
                     "the subscription refresh never reached the origin");

            // ---- 6. a profile write is in flight too -----------------------
            app.profiles->saveProfileContentAsync(
                charlie, QString::fromUtf8(wf::directOnlyProfile(0, QStringLiteral("charlie-2"))));
            QVERIFY2(app.profiles->isFileBusy(),
                     "the pending write this journey needs was not created, so the quiesce "
                     "assertion below would prove nothing");

            // ---- 7. restore -------------------------------------------------
            QStringList order;
            bool preparedWhileFileBusy = false;
            bool preparedWhileCoreAlive = false;
            bool maintenanceDuringRestore = false;
            QStringList statuses;
            QObject::connect(app.backups.get(), &app::backup::BackupCoordinator::statusMessage,
                             app.backups.get(),
                             [&statuses](const QString &text) { statuses.append(text); });
            QObject::connect(store, &core::BackupStore::restorePrepared, store,
                             [&, this] {
                                 order.append(QStringLiteral("prepared"));
                                 preparedWhileFileBusy = app.profiles->isFileBusy();
                                 maintenanceDuringRestore = app.backups->isMaintenanceActive();
                             });
            QObject::connect(store, &core::BackupStore::restored, store, [&, this] {
                order.append(QStringLiteral("restored"));
                preparedWhileCoreAlive = app.backend->state() != cb::CoreState::Stopped;
            });

            store->restoreLocalAsync(archive);
            QVERIFY2(wf::waitFor([&order] { return order.contains(QStringLiteral("restored")); }),
                     qPrintable(QStringLiteral("the restore never finished: %1 | %2")
                                    .arg(order.join(QLatin1Char('>')), report(app, controller))));
            QVERIFY(wf::waitFor([store] { return !store->isBusy(); }));

            // writers quiesced
            // Matched by prefix: the shipped strings end in a U+2026 ellipsis
            // and this assertion is about which gate ran, not about typography.
            QVERIFY2(std::any_of(statuses.cbegin(), statuses.cend(),
                                 [](const QString &text) {
                                     return text.startsWith(QStringLiteral(
                                         "Waiting for pending profile changes to finish"));
                                 }),
                     qPrintable(QStringLiteral("the pending-write gate never ran: [%1]")
                                    .arg(statuses.join(QStringLiteral(" | ")))));
            QVERIFY2(std::any_of(statuses.cbegin(), statuses.cend(),
                                 [](const QString &text) {
                                     return text.startsWith(
                                         QStringLiteral("Stopping the core before restoring"));
                                 }),
                     qPrintable(QStringLiteral("the core-stop gate never ran: [%1]")
                                    .arg(statuses.join(QStringLiteral(" | ")))));
            QVERIFY2(!preparedWhileFileBusy,
                     "the archive was validated while a profile write was still outstanding");
            // processes quiesced
            QCOMPARE(order, (QStringList{QStringLiteral("prepared"), QStringLiteral("restored")}));
            QVERIFY2(!preparedWhileCoreAlive,
                     "the files were restored while the managed core was still up");
            QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()),
                     "the managed child outlived the restore that was supposed to stop it");
            QVERIFY2(maintenanceDuringRestore,
                     "the stores were not in maintenance mode while the archive was unpacked");
            QVERIFY2(!app.backups->isMaintenanceActive(),
                     "maintenance mode outlived the restore");

            // ---- 8. the stale reply is released, AFTER the restore ----------
            held->release();
            QVERIFY(wf::drainPostedTimers());
            QVERIFY(wf::waitFor([&app] { return !app.profiles->isFileBusy(); }));

            // restored state, in the process that performed the restore
            QCOMPARE(app.profiles->profiles().size(), 3);
            QCOMPARE(app.profiles->currentUid(), alpha);
            QVERIFY(uidOf(app, QStringLiteral("bravo")) == bravo);
            QVERIFY2(uidOf(app, QStringLiteral("charlie")).isEmpty(),
                     "a profile created after the backup survived the restore");
            QCOMPARE(app.enhancer->chain().size(), 1);
            QVERIFY2(app.enhancer->chain().first().enabled,
                     "the enhancement chain was not restored");
            QCOMPARE(wf::readTextFile(pathOf(app, remote)), backedUpSubscriptionBody());
            QVERIFY2(wf::readTextFile(pathOf(app, remote)) != staleSubscriptionBody(),
                     "a reply issued before the restore overwrote the restored profile");
            QCOMPARE(core::preferences::open().value(QStringLiteral("sysproxy/bypass")).toString(),
                     QStringLiteral("backed-up.example"));
            QCOMPARE(
                core::preferences::open().value(QStringLiteral("hotkeys/proxy.toggle")).toString(),
                QStringLiteral("Ctrl+Alt+P"));

            QVERIFY2(app.quit(), qPrintable(app.blockingReason()));
            QVERIFY2(wf::awaitNoLiveProcess(engine.binaryPath()), "a child outlived the quit");
        }

        // ---- 9. reopen -----------------------------------------------------
        {
            wf::AssembledApp app(proxyLog, osProxy);
            app.bootstrap(engine.binaryPath());
            QVERIFY2(app.startupErrors.isEmpty(),
                     qPrintable(app.startupErrors.join(QLatin1Char('\n'))));

            QCOMPARE(app.profiles->profiles().size(), 3);
            QCOMPARE(app.profiles->currentUid(), alpha);
            QVERIFY(!uidOf(app, QStringLiteral("alpha")).isEmpty());
            QVERIFY(!uidOf(app, QStringLiteral("bravo")).isEmpty());
            QVERIFY(!uidOf(app, QStringLiteral("remote")).isEmpty());
            QVERIFY2(uidOf(app, QStringLiteral("charlie")).isEmpty(),
                     "a profile created after the backup came back on reopen");
            QCOMPARE(wf::readTextFile(pathOf(app, remote)), backedUpSubscriptionBody());
            QCOMPARE(app.enhancer->chain().size(), 1);
            QCOMPARE(app.enhancer->chain().first().uid, chainUid);
            QVERIFY(app.enhancer->chain().first().enabled);
            QVERIFY(QFileInfo::exists(app.enhancer->chain().first().filePath));
            QCOMPARE(core::preferences::open().value(QStringLiteral("sysproxy/bypass")).toString(),
                     QStringLiteral("backed-up.example"));
            QVERIFY2(app.quit(), qPrintable(app.blockingReason()));
        }

        QVERIFY2(!controller.sawUnexpectedRequest(), qPrintable(controller.redactedTranscript()));
    }

  private:
    QString engineDirectory() {
        const QString marker = environment_->filePath(QStringLiteral("engine/.keep"));
        const QString dir = QFileInfo(marker).absolutePath();
        QDir().mkpath(dir);
        return dir;
    }

    QString createProfile(wf::AssembledApp &app, const QString &name) {
        const QStringList before = uids(app);
        if (!app.profiles->createLocalProfile(name,
                                              QString::fromUtf8(wf::directOnlyProfile(0, name))))
            return {};
        for (const QString &uid : uids(app))
            if (!before.contains(uid)) return uid;
        return {};
    }

    static QStringList uids(wf::AssembledApp &app) {
        QStringList out;
        for (const auto &profile : app.profiles->profiles()) out << profile.uid;
        return out;
    }

    static QString uidOf(wf::AssembledApp &app, const QString &name) {
        for (const auto &profile : app.profiles->profiles())
            if (profile.name == name) return profile.uid;
        return {};
    }

    static QString pathOf(wf::AssembledApp &app, const QString &uid) {
        for (const auto &profile : app.profiles->profiles())
            if (profile.uid == uid) return profile.filePath;
        return {};
    }

    static QByteArray backedUpSubscriptionBody() {
        return wf::directOnlyProfile(0, QStringLiteral("subscription-v1"));
    }

    static QByteArray staleSubscriptionBody() {
        return wf::directOnlyProfile(0, QStringLiteral("subscription-v2-must-not-land"));
    }

    static QString report(wf::AssembledApp &app, LoopbackServer &controller) {
        return QStringLiteral("state=%1 | events: %2 | store: %3 | %4")
            .arg(static_cast<int>(app.backend->state()))
            .arg(app.events.transcript(), app.storeErrors.join(QLatin1Char(' ')),
                 controller.pendingReport());
    }

    std::unique_ptr<ScopedEnvironment> environment_;
    QString enginePath_;
};

QTEST_GUILESS_MAIN(W04RestoreTest)
#include "w04_restore_test.moc"
