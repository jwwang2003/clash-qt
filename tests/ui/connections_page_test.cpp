// Connections page: rates and closed history accumulate while the page is hidden,
// the selected connection id survives a reordered snapshot, per-connection rates
// derive from counter deltas and reset on rollback, closed history is bounded with
// a frozen duration, and the details dialog copies exactly its own text.
//
// Partition of the former `data-pages` suite (tests/ui/data_pages_test.cpp). Cases
// are carried over verbatim; see tests/README.md for the full original-to-new map.
//
// connectionDetailsCanBeCopied() writes the process clipboard. That is a
// process-global side effect, so this suite must not share an executable with a
// suite that reads the clipboard.
#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <memory>

#include "core/backend/backend_bridge.h"
#include "support/backend/fake_backend.h"
#include "ui/pages/connections/connections_page.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class ConnectionsPageTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("conns"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    void hiddenConnectionsStillCaptureRatesAndClosedHistory() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ConnectionsPage page(&bridge);
        page.show();
        core::backend::Connection first;
        first.id = "first"; first.host = "first.example";
        bridge.connectionsUpdated({first}, 0, 0);
        auto *model = page.findChild<ui::ConnectionModel *>();
        QTRY_COMPARE(model->rowCount(), 1);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        page.hide();
        core::backend::Connection second;
        second.id = "second"; second.host = "second.example";
        bridge.connectionsUpdated({second}, 0, 0);
        QTest::qWait(25);
        second.upload = 2000;
        bridge.connectionsUpdated({second}, 2000, 0);
        QTest::qWait(125);
        QCOMPARE(resets.size(), 0);
        page.show();
        QTRY_COMPARE(model->idAt(0), QString("second"));
        QVERIFY(model->index(0, ui::ConnectionModel::UploadRate).data(Qt::UserRole).toDouble() > 0);
        page.findChild<QComboBox *>()->setCurrentIndex(1);
        QCOMPARE(model->idAt(0), QString("first"));
        resets.clear();
        second.upload += 500;
        bridge.connectionsUpdated({second}, 2500, 0);
        QTest::qWait(125);
        QCOMPARE(resets.size(), 0);
    }

    void connectionSnapshotPreservesSelectedId() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ConnectionsPage page(&bridge);
        page.show();
        core::backend::Connection a, b;
        a.id = "a"; a.host = "a.example";
        b.id = "b"; b.host = "b.example";
        bridge.connectionsUpdated({a, b}, 0, 0);
        auto *view = page.findChild<QTableView *>();
        QTRY_COMPARE(view->model()->rowCount(), 2);
        view->sortByColumn(0, Qt::AscendingOrder);
        view->setCurrentIndex(view->model()->index(1, 0));
        QCOMPARE(view->currentIndex().data().toString(), QString("b.example"));
        bridge.connectionsUpdated({b, a}, 100, 200);
        QTRY_COMPARE(page.findChild<ui::ConnectionModel *>()->idAt(0), QString("b"));
        QCOMPARE(view->currentIndex().data().toString(), QString("b.example"));
    }

    void connectionRatesSortAndResetOnCounterRollback() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ConnectionsPage page(&bridge);
        page.show();
        core::backend::Connection slow, fast;
        slow.id = "slow"; slow.host = "slow.example";
        fast.id = "fast"; fast.host = "fast.example";
        bridge.connectionsUpdated({slow, fast}, 0, 0);
        QTest::qWait(25);
        slow.upload = 100; slow.download = 200;
        fast.upload = 10000; fast.download = 20000;
        bridge.connectionsUpdated({slow, fast}, 10100, 20200);
        auto *model = page.findChild<ui::ConnectionModel *>();
        QVERIFY(model);
        QTRY_VERIFY(model->index(0, ui::ConnectionModel::UploadRate).data(Qt::UserRole).toDouble() > 0);
        const double first = model->index(0, ui::ConnectionModel::UploadRate).data(Qt::UserRole).toDouble();
        const double second = model->index(1, ui::ConnectionModel::UploadRate).data(Qt::UserRole).toDouble();
        QVERIFY(first > 0);
        QVERIFY(second > first);
        QCOMPARE(model->index(0, ui::ConnectionModel::DownloadRate).data(Qt::UserRole).toDouble(), first * 2);
        auto *view = page.findChild<QTableView *>();
        view->sortByColumn(ui::ConnectionModel::UploadRate, Qt::DescendingOrder);
        QCOMPARE(view->model()->index(0, 0).data().toString(), QString("fast.example"));
        QTest::qWait(25);
        slow.upload = slow.download = 0;
        bridge.connectionsUpdated({slow, fast}, 0, 0);
        QTRY_COMPARE(model->index(0, ui::ConnectionModel::UploadRate).data(Qt::UserRole).toDouble(), 0.0);
    }

    void closedHistoryIsBoundedAndHasFrozenDuration() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ConnectionsPage page(&bridge);
        page.show();
        QVector<core::backend::Connection> connections;
        for (int i = 0; i < 600; ++i) {
            core::backend::Connection connection;
            connection.id = QString::number(i);
            connection.host = QString("host%1.example").arg(i);
            connection.start = QDateTime::currentDateTime().addSecs(-3600);
            connections.append(connection);
        }
        bridge.connectionsUpdated(connections, 0, 0);
        bridge.connectionsUpdated({}, 0, 0);
        page.findChild<QComboBox *>()->setCurrentIndex(1);
        auto *model = page.findChild<ui::ConnectionModel *>();
        QCOMPARE(model->rowCount(), 500);
        QVERIFY(model->connectionAt(0)->end.isValid());
        const auto duration = model->index(0, ui::ConnectionModel::Duration).data(Qt::UserRole);
        QTest::qWait(25);
        QCOMPARE(model->index(0, ui::ConnectionModel::Duration).data(Qt::UserRole), duration);
        bridge.endpointChanged({}, core::backend::Ownership::None);
        QCOMPARE(model->rowCount(), 0);
    }

    void connectionDetailsCanBeCopied() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::ConnectionsPage page(&bridge);
        page.show();
        core::backend::Connection connection;
        connection.id = "detail-id";
        connection.host = "test.example";
        connection.sourceIp = "127.0.0.1";
        connection.sourcePort = "3210";
        connection.processPath = "/Applications/Test App.app/Contents/MacOS/test";
        connection.destinationPort = "443";
        bridge.connectionsUpdated({connection}, 0, 0);
        auto *view = page.findChild<QTableView *>();
        QTRY_COMPARE(view->model()->rowCount(), 1);
        view->doubleClicked(view->model()->index(0, 0));
        auto *dialog = page.findChild<QDialog *>();
        QVERIFY(dialog);
        auto *text = dialog->findChild<QPlainTextEdit *>();
        QVERIFY(text->toPlainText().contains("127.0.0.1:3210"));
        QVERIFY(text->toPlainText().contains(connection.processPath));
        bool copied = false;
        for (auto *button : dialog->findChildren<QPushButton *>()) {
            if (button->text() != "Copy Details") continue;
            button->click();
            copied = true;
        }
        QVERIFY(copied);
        QCOMPARE(QApplication::clipboard()->text(), text->toPlainText());
        dialog->reject();
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(ConnectionsPageTest)
#include "connections_page_test.moc"
