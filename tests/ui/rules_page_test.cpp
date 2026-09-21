// Rules page: rules keep the controller's routing order rather than being sorted
// alphabetically, and the type filter narrows to the matching rule.
//
// Partition of the former `data-pages` suite (tests/ui/data_pages_test.cpp). The
// case is carried over verbatim; see tests/README.md for the original-to-new map.
#include <QtTest>
#include <QApplication>
#include <QLineEdit>
#include <QTableView>
#include <memory>

#include "core/mihomo/mihomo_client.h"
#include "ui/pages/rules/rules_page.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class RulesPageTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("rules"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    void rulesKeepRoutingOrderAndFilterType() {
        core::MihomoClient client;
        ui::RulesPage page(&client);
        client.rulesUpdated({{"ZZZ", "first", "DIRECT"}, {"AAA", "last", "REJECT"}});
        auto *view = page.findChild<QTableView *>();
        QCOMPARE(view->model()->index(0, 0).data().toString(), QString("ZZZ"));
        page.findChild<QLineEdit *>()->setText("reject");
        QCOMPARE(view->model()->rowCount(), 1);
        QCOMPARE(view->model()->index(0, 1).data().toString(), QString("last"));
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(RulesPageTest)
#include "rules_page_test.moc"
