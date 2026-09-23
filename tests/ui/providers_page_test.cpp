// Providers page: rows are not built before the first show, an unchanged payload
// causes no model reset, and a change arriving while hidden is applied on show.
//
// Partition of the former `data-pages` suite (tests/ui/data_pages_test.cpp). The
// case is carried over verbatim; see tests/README.md for the original-to-new map.
//
// The request gate is HELD for the same reason the QTcpServer fixture used to be
// here and answer nothing: the page fetches on every show, and a fetch that
// completes would deliver the fake's own (empty) provider set over the payload
// the case just published.
#include <QtTest>
#include <QApplication>
#include <QTableView>
#include <memory>

#include "core/backend/backend_bridge.h"
#include "support/backend/fake_backend.h"
#include "ui/pages/providers/providers_page.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class ProvidersPageTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("provdrs"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    void providersSkipHiddenAndUnchangedRebuilds() {
        testsupport::backend::FakeBackend backend;
        backend.setRequestGate(testsupport::backend::Gate::Held);
        core::backend::BackendBridge bridge(backend);
        ui::ProvidersPage page(&bridge);
        auto *view = page.findChild<QTableView *>();
        QVERIFY(view);
        core::backend::Provider provider;
        provider.name = "Test provider";
        provider.vehicle = "HTTP";
        provider.count = 25;
        bridge.providersReceived(false, {provider});
        QCOMPARE(view->model()->rowCount(), 0);
        page.show();
        QTRY_COMPARE(view->model()->rowCount(), 1);
        QSignalSpy resets(view->model(), &QAbstractItemModel::modelReset);
        bridge.providersReceived(false, {provider});
        QTest::qWait(30);
        QCOMPARE(resets.size(), 0);
        page.hide();
        provider.count = 30;
        bridge.providersReceived(false, {provider});
        QCOMPARE(view->model()->index(0, 3).data().toInt(), 25);
        page.show();
        QTRY_COMPARE(view->model()->index(0, 3).data().toInt(), 30);
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(ProvidersPageTest)
#include "providers_page_test.moc"
