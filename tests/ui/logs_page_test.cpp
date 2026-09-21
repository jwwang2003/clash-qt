// Logs page: the filter applies to already-received history in both directions, a
// payload containing markup is not escaped away, an endpoint change clears the
// buffer, and the log font is no smaller than the page font.
//
// Partition of the former `data-pages` suite (tests/ui/data_pages_test.cpp). The
// case is carried over verbatim; see tests/README.md for the original-to-new map.
#include <QtTest>
#include <QApplication>
#include <QDateTime>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <memory>

#include "core/backend/backend_bridge.h"
#include "support/backend/fake_backend.h"
#include "ui/pages/logs/logs_page.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class LogsPageTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("logs"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    void logsFilterExistingHistoryAndEscapePayload() {
        testsupport::backend::FakeBackend backend;
        core::backend::BackendBridge bridge(backend);
        ui::LogsPage page(&bridge);
        bridge.logReceived({"info", "<hello>", QDateTime::currentDateTime()});
        bridge.logReceived({"error", "failed", QDateTime::currentDateTime()});
        auto *view = page.findChild<QPlainTextEdit *>();
        QVERIFY(view->toPlainText().contains("<hello>"));
        auto *filter = page.findChild<QLineEdit *>();
        filter->setText("failed");
        QTRY_VERIFY(!view->toPlainText().contains("<hello>"));
        QVERIFY(view->toPlainText().contains("failed"));
        filter->clear();
        QTRY_VERIFY(view->toPlainText().contains("<hello>"));
        bridge.endpointChanged({}, core::backend::Ownership::None);
        QVERIFY(view->toPlainText().isEmpty());
        filter->setText("hello");
        QVERIFY(view->toPlainText().isEmpty());
        QVERIFY(view->font().pointSizeF() >= page.font().pointSizeF());
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(LogsPageTest)
#include "logs_page_test.moc"
