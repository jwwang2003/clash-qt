// Proxies page: an empty snapshot clears both lists and disables the latency
// action, sorting and filtering behave, and repeated or hidden snapshots do not
// rebuild the model.
//
// Partition of the former `data-pages` suite (tests/ui/data_pages_test.cpp). Cases
// are carried over verbatim; see tests/README.md for the full original-to-new map.
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <memory>

#include "core/backend/backend_bridge.h"
#include "support/backend/fake_backend.h"
#include "ui/pages/proxies/proxies_page.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class ProxiesPageTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("proxies"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    void emptyProxySnapshotClearsMembersAndAction() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ProxiesPage page(&bridge);
        page.show();
        bridge.proxiesUpdated({core::backend::ProxyGroup{"Choose", "Selector", "Node", {"Node"}}},
                              {{"Node", core::backend::ProxyNode{"Node", "Direct", 20}}});
        const auto lists = page.findChildren<QListWidget *>();
        QCOMPARE(lists.size(), 2);
        QTRY_COMPARE(lists[0]->count(), 1);
        QCOMPARE(lists[1]->count(), 1);
        bridge.proxiesUpdated({}, {});
        QTRY_COMPARE(lists[0]->count(), 0);
        QCOMPARE(lists[1]->count(), 0);
        for (auto *button : page.findChildren<QPushButton *>())
            if (button->text() == "Test Latency") QVERIFY(!button->isEnabled());
    }

    void proxyFilterAndLatencySort() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ProxiesPage page(&bridge);
        page.show();
        bridge.proxiesUpdated({core::backend::ProxyGroup{"Choose", "Selector", "Fast", {"Slow", "Fast", "Untested"}}},
                              {{"Slow", core::backend::ProxyNode{"Slow", "Direct", 200}},
                               {"Fast", core::backend::ProxyNode{"Fast", "Direct", 10}},
                               {"Untested", core::backend::ProxyNode{"Untested", "Direct", -1}}});
        auto *nodes = page.findChild<QListWidget *>("proxyNodes");
        QVERIFY(nodes);
        QTRY_COMPARE(nodes->count(), 3);
        page.findChild<QComboBox *>()->setCurrentIndex(2);
        QCOMPARE(nodes->item(0)->text(), QString("Fast"));
        QCOMPARE(nodes->item(2)->text(), QString("Untested"));
        page.findChild<QLineEdit *>()->setText("slow");
        QTRY_COMPARE(nodes->count(), 1);
        QCOMPARE(nodes->item(0)->text(), QString("Slow"));
    }

    void proxySnapshotsSkipUnchangedAndHiddenRebuilds() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ProxiesPage page(&bridge);
        page.show();
        QVector<core::backend::ProxyGroup> groups{{"Choose", "Selector", "Node", {"Node"}}};
        QHash<QString, core::backend::ProxyNode> nodes{{"Node", {"Node", "Direct", 20}}};
        auto *list = page.findChild<QListWidget *>("proxyNodes");
        bridge.proxiesUpdated(groups, nodes);
        QTRY_COMPARE(list->count(), 1);
        QSignalSpy resets(list->model(), &QAbstractItemModel::modelReset);
        for (int i = 0; i < 10; ++i) bridge.proxiesUpdated(groups, nodes);
        QTest::qWait(30);
        QCOMPARE(resets.size(), 0);
        page.hide();
        groups[0].all << "Next";
        nodes.insert("Next", {"Next", "Direct", 30});
        bridge.proxiesUpdated(groups, nodes);
        QTest::qWait(30);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(list->count(), 1);
        page.show();
        QTRY_COMPARE(list->count(), 2);
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(ProxiesPageTest)
#include "proxies_page_test.moc"
