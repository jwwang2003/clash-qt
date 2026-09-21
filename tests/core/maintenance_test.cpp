// Maintenance mode as a coordinator contract: entering it must quiesce every
// writer -- the profile store and the enhancement chain together -- before a
// backup restore may proceed, and must cancel runtime generation already running.
//
// Partition of the former `runtime` suite (tests/core/runtime_test.cpp). Cases are
// carried over verbatim; see tests/README.md for the full original-to-new map.
//
// Logical home: `tests/app/runtime/`. It is parked under tests/core/ because the
// application package does not own a test directory yet; moving it is a path
// change only, with no edit to this file.
#include <QtTest>
#include <QSignalSpy>
#include <memory>

#include "core/config/enhance/config_enhancer.h"
#include "core/profiles/profile_store.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"

class RuntimeMaintenanceTest : public QObject {
    Q_OBJECT
private slots:
    // One scoped environment per test function, as the original suite had one
    // QTemporaryDir per test function. ScopedEnvironment additionally *restores*
    // any CLASH_QT_DATA_DIR the developer had exported, where the original
    // cleanup() unset it and lost it for the rest of the process.
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("maint"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() { environment_.reset(); }
    void maintenanceCancelsFileWritesBeforeRestoreCanProceed() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        const QString input = environment_->filePath("pending.yaml");
        testsupport::writeFile(input, "proxies: []\n");
        store.importFromFileAsync(input);
        enhancer.importItemAsync(input, core::ChainKind::Merge);
        store.setMaintenanceMode(true);
        enhancer.setMaintenanceMode(true);
        QTRY_VERIFY(!store.isFileBusy());
        QTRY_VERIFY(!enhancer.isFileBusy());
        QVERIFY(store.profiles().isEmpty());
        QVERIFY(enhancer.chain().isEmpty());
        store.load();
        enhancer.load();
        QVERIFY(store.profiles().isEmpty());
        QVERIFY(enhancer.chain().isEmpty());
    }
    void maintenanceBlocksMutationsAndCancelsRuntimeWork() {
        core::ProfileStore store;
        core::ConfigEnhancer enhancer;
        store.setEnhancer(&enhancer);
        QVERIFY(store.createLocalProfile("protected", "proxies: []\n"));
        enhancer.addScript("slow");
        const auto item = enhancer.chain().first();
        QVERIFY(enhancer.saveItemContent(item.uid, "function main(c) { while(true) {} return c; }"));
        const QString uid = store.currentUid();
        QSignalSpy ready(&store, &core::ProfileStore::runtimeConfigReady);
        store.requestRuntimeConfig();
        QTest::qWait(30);
        store.setMaintenanceMode(true);
        enhancer.setMaintenanceMode(true);
        QVERIFY(!store.saveProfileContent(uid, "proxies: []\nmode: direct\n"));
        QVERIFY(!store.createLocalProfile("blocked", "proxies: []\n"));
        QVERIFY(!enhancer.saveItemContent(item.uid, "function main(c) { return c; }"));
        enhancer.removeItem(item.uid);
        store.removeProfile(uid);
        QCOMPARE(store.profiles().size(), 1);
        QCOMPARE(enhancer.chain().size(), 1);
        QTRY_VERIFY_WITH_TIMEOUT(!store.isRuntimeBusy(), 1000);
        QVERIFY(ready.isEmpty());
        store.load();
        enhancer.load();
        QCOMPARE(store.currentUid(), uid);
        store.setMaintenanceMode(false);
        enhancer.setMaintenanceMode(false);
        QVERIFY(store.saveProfileContent(uid, "proxies: []\nmode: direct\n"));
        QVERIFY(enhancer.saveItemContent(item.uid, "function main(c) { return c; }"));
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(RuntimeMaintenanceTest)
#include "maintenance_test.moc"
