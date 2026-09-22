// ConfigEnhancer::snapshotChain -- the legacy enhancement running on values
// instead of on paths.
//
// The ChainItem overload opens step N's file immediately before running step
// N. On a chain whose first step is a slow script that is a window, several
// seconds wide, in which a saved merge fragment is picked up halfway through a
// run: the generated configuration then corresponds to no single state of the
// chain. Snapshotting closes the window, and these cases are what say the
// snapshot is actually frozen rather than merely named that.
//
// Contract: config-r1.
#include <QtTest>
#include <memory>
#include <yaml-cpp/yaml.h>

#include "core/config/enhance/config_enhancer.h"
#include "support/fixture_files.h"
#include "support/scoped_environment.h"

class ChainSnapshotTest : public QObject {
    Q_OBJECT
private slots:
    void init() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("snap"));
        QVERIFY2(environment_->isValid(), qPrintable(environment_->errorString()));
    }
    void cleanup() { environment_.reset(); }

    // The property, stated as a difference: an edit after the snapshot is
    // invisible to the snapshot and visible to the live chain. Asserting only
    // the first half would pass against a snapshot that was never taken.
    void asnapshotFreezesContentsAndTheLiveChainDoesNot() {
        core::ConfigEnhancer enhancer;
        enhancer.addMerge("fragment");
        const QString path = enhancer.chain().first().filePath;
        testsupport::writeFile(path, "ipv6: true\n");

        const core::ChainSnapshot frozen = enhancer.snapshot();
        testsupport::writeFile(path, "ipv6: false\n");

        const auto snapshotted =
            core::ConfigEnhancer::applyChain("proxies: []\n", "name", frozen);
        QVERIFY2(snapshotted.error.isEmpty(), qPrintable(snapshotted.error));
        QVERIFY(YAML::Load(snapshotted.yaml.toStdString())["ipv6"].as<bool>());

        const auto live =
            core::ConfigEnhancer::applyChain("proxies: []\n", "name", enhancer.chain());
        QVERIFY2(live.error.isEmpty(), qPrintable(live.error));
        QVERIFY(!YAML::Load(live.yaml.toStdString())["ipv6"].as<bool>());
    }

    // Deleting a file after the snapshot cannot turn a working chain into a
    // failing one: there is nothing left to open.
    void asnapshotSurvivesTheFileBeingRemoved() {
        core::ConfigEnhancer enhancer;
        enhancer.addMerge("fragment");
        const QString path = enhancer.chain().first().filePath;
        testsupport::writeFile(path, "ipv6: true\n");
        const core::ChainSnapshot frozen = enhancer.snapshot();
        QVERIFY(QFile::remove(path));

        const auto result = core::ConfigEnhancer::applyChain("proxies: []\n", "name", frozen);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(YAML::Load(result.yaml.toStdString())["ipv6"].as<bool>());
    }

    // Whether a step runs is decided at snapshot time too, so toggling it
    // afterwards cannot make a step run against contents that were never read.
    void whetherAStepRunsIsPartOfTheSnapshot() {
        core::ConfigEnhancer enhancer;
        enhancer.addMerge("fragment");
        const auto item = enhancer.chain().first();
        testsupport::writeFile(item.filePath, "ipv6: true\n");
        enhancer.setEnabled(item.uid, false);

        const core::ChainSnapshot frozen = enhancer.snapshot();
        QCOMPARE(frozen.size(), 1);
        QVERIFY(!frozen.first().item.enabled);
        QVERIFY(frozen.first().contents.isEmpty());  // a skipped step is not read

        enhancer.setEnabled(item.uid, true);
        const auto result = core::ConfigEnhancer::applyChain("proxies: []\n", "name", frozen);
        QVERIFY(result.error.isEmpty());
        QVERIFY(!YAML::Load(result.yaml.toStdString())["ipv6"]);
    }

    // A read failure is still attributed to the step it belongs to, and the
    // chain still runs -- the semantics the read-at-the-last-moment version had.
    void aunreadableStepFailsThatStepAndOnlyThatStep() {
        core::ConfigEnhancer enhancer;
        enhancer.addMerge("missing");
        enhancer.addMerge("present");
        const auto items = enhancer.chain();
        QVERIFY(QFile::remove(items.first().filePath));
        testsupport::writeFile(items.last().filePath, "ipv6: true\n");

        const auto result =
            core::ConfigEnhancer::applyChain("proxies: []\nmode: rule\n", "name",
                                             enhancer.snapshot());
        QVERIFY(result.error.contains("missing"));
        const YAML::Node config = YAML::Load(result.yaml.toStdString());
        QVERIFY(config["ipv6"].as<bool>());  // the later step still ran
        QCOMPARE(config["mode"].as<std::string>(), std::string("rule"));
    }

    // The two overloads are the same enhancement. Whatever the ChainItem form
    // still guarantees -- script failure semantics, YAML tags, ordering -- it
    // guarantees by delegating here, and this is what says so.
    void bothOverloadsAgreeOnAnUnchangingChain() {
        core::ConfigEnhancer enhancer;
        enhancer.addScript("broken");
        testsupport::writeFile(
            enhancer.chain().first().filePath,
            "function main(c) { c.mode='global'; console.log('before'); throw Error('oops'); }");
        enhancer.addMerge("after");
        testsupport::writeFile(enhancer.chain().last().filePath, "IPV6: true\n");
        const QString source = "proxies: []\nmode: rule\npassword: !!str 123\n";

        const auto viaItems = core::ConfigEnhancer::applyChain(source, "name", enhancer.chain());
        const auto viaSnapshot =
            core::ConfigEnhancer::applyChain(source, "name", enhancer.snapshot());
        QCOMPARE(viaSnapshot.yaml, viaItems.yaml);
        QCOMPARE(viaSnapshot.error, viaItems.error);
        QCOMPARE(viaSnapshot.logs, viaItems.logs);
        QVERIFY(viaSnapshot.error.contains("oops"));
        QVERIFY(viaSnapshot.logs.join('\n').contains("before"));
        const YAML::Node config = YAML::Load(viaSnapshot.yaml.toStdString());
        QCOMPARE(config["mode"].as<std::string>(), std::string("rule"));
        QVERIFY(config["ipv6"].as<bool>());
        QCOMPARE(config["password"].Tag(), std::string("!"));
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_GUILESS_MAIN(ChainSnapshotTest)
#include "chain_snapshot_test.moc"
