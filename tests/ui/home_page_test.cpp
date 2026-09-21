// Home / overview page layout: the three cards stack without overlap at both the
// minimum and the default window size, the traffic chart and DNS controls keep
// their vertical order and stay reachable by scrolling, and a wheel event landing
// on the native Quick window scrolls the page instead of being swallowed.
//
// Partition of the former `data-pages` suite (tests/ui/data_pages_test.cpp). Cases
// are carried over verbatim; see tests/README.md for the full original-to-new map.
//
// CLASH_QT_AUDIT_IMAGES, honoured by homeCardsDoNotOverlap(), is an optional
// artefact dump and not a gate: unset, the case still asserts everything.
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QQuickView>
#include <QScrollArea>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtGraphs/QAreaSeries>
#include <algorithm>
#include <memory>

#include "core/mihomo/mihomo_client.h"
#include "ui/pages/overview/home_page.h"
#include "ui/theme/theme.h"
#include "ui/widgets/settings_section.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class HomePageTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("home"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    void homeCardsDoNotOverlap_data() {
        QTest::addColumn<QSize>("pageSize");
        QTest::newRow("minimum-684x450") << QSize(684, 450);
        QTest::newRow("default-944x590") << QSize(944, 590);
    }

    void homeCardsDoNotOverlap() {
        // Regression: the native Mac audit found chart/totals and DNS/cache controls
        // overlapping when a fixed-height window compressed the old card layouts.
        // This only renders synthetic test widgets; it never captures the OS screen.
        QFETCH(QSize, pageSize);
        ui::theme::install();
        core::MihomoClient client;
        ui::HomePage page(&client);
        QFont font = page.font();
        font.setPointSizeF(13);
        page.setFont(font);
        page.resize(pageSize);
        core::BaseConfig config;
        config.mode = "rule";
        config.mixedPort = 7897;
        config.httpPort = 7890;
        config.socksPort = 7891;
        client.configReceived(config);
        client.versionReceived("v1.19.0 · synthetic audit fixture");
        client.memorySample(42 * 1024 * 1024, 0);
        for (int i = 0; i < 60; ++i)
            client.trafficSample(10000 + ((i * 17) % 31) * 1300, 40000 + ((i * 13) % 29) * 12000);
        client.connectionsUpdated(QVector<core::Connection>(12), 12500000, 482000000);
        client.dnsQueryFinished("example.com", QJsonObject{
            {"Status", 0},
            {"Answer", QJsonArray{QJsonObject{{"name", "example.com."}, {"TTL", 60}, {"data", "192.0.2.10"}}}}
        }, {});
        client.connectedChanged(true);
        page.show();
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();
        QCOMPARE(page.size(), pageSize);
        auto *scroll = page.findChild<QScrollArea *>();
        QVERIFY(scroll);
        QWidget *body = scroll->widget();
        QVERIFY(body);
        QVERIFY2(body->width() <= scroll->viewport()->width(), "Home content must fit without hidden horizontal overflow");
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        auto cards = body->findChildren<ui::SettingsSection *>(QString(), Qt::FindDirectChildrenOnly);
        QCOMPARE(cards.size(), 3);
        std::sort(cards.begin(), cards.end(), [](const QWidget *a, const QWidget *b) {
            return a->geometry().top() < b->geometry().top();
        });
        for (int i = 0; i < cards.size(); ++i) {
            QVERIFY(body->rect().contains(cards[i]->geometry()));
            if (i) QVERIFY2(cards[i - 1]->geometry().bottom() < cards[i]->geometry().top(),
                            "Overview cards overlap instead of scrolling");
        }
        auto *chart = page.findChild<QWidget *>("trafficChart");
        auto *rates = page.findChild<QLabel *>("trafficRates");
        auto *totals = page.findChild<QLabel *>("trafficTotals");
        auto *result = page.findChild<QPlainTextEdit *>("dnsResult");
        auto *flush = page.findChild<QPushButton *>("flushDnsButton");
        auto *flushFake = page.findChild<QPushButton *>("flushFakeIpButton");
        QVERIFY(chart && rates && totals && result && flush && flushFake);
        QVERIFY(chart->height() >= 280);
        auto *graphView = page.windowHandle()->findChild<QQuickView *>("trafficGraphsView");
        QVERIFY(graphView);
        QTRY_COMPARE(graphView->status(), QQuickView::Ready);
        QCOMPARE(chart->findChildren<QAreaSeries *>().size(), 2);
        QVERIFY(rates->geometry().bottom() < chart->geometry().top());
        QVERIFY(chart->geometry().bottom() < totals->geometry().top());
        QVERIFY(chart->parentWidget()->rect().contains(totals->geometry()));
        QCOMPARE(result->height(), 110);
        QVERIFY(result->geometry().bottom() < flush->geometry().top());
        QVERIFY(result->geometry().bottom() < flushFake->geometry().top());
        QVERIFY(!flush->geometry().intersects(flushFake->geometry()));
        QVERIFY(result->parentWidget()->rect().contains(flushFake->geometry()));

        const QString output = qEnvironmentVariable("CLASH_QT_AUDIT_IMAGES");
        auto render = [&](QWidget *widget, const QString &suffix) {
            if (output.isEmpty()) return true;
            if (!QDir().mkpath(output)) return false;
            QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(widget->palette().color(QPalette::Window));
            widget->render(&image);
            // QWidget::render does not include native child windows. Composite
            // the Quick scene explicitly; this still captures only our fixture.
            const QImage plotImage = graphView->grabWindow();
            if (!plotImage.isNull()) {
                auto *container = chart->findChild<QWidget *>("trafficGraphsContainer");
                QPainter painter(&image);
                const QPoint origin = widget->mapFromGlobal(container->mapToGlobal(QPoint()));
                QRect target(origin, container->size());
                QRect clip = target;
                for (QWidget *parent = container->parentWidget(); parent && parent != widget;
                     parent = parent->parentWidget()) {
                    if (!widget->isAncestorOf(parent)) break;
                    clip &= QRect(widget->mapFromGlobal(parent->mapToGlobal(QPoint())), parent->size());
                }
                painter.setClipRect(clip);
                painter.drawImage(target, plotImage);
            }
            return image.save(QDir(output).filePath(QString("home-synthetic-%1-%2.png")
                                  .arg(QString::fromLatin1(QTest::currentDataTag()), suffix)));
        };
        QVERIFY(render(&page, "top"));
        QVERIFY(render(body, "full-content"));
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        QCoreApplication::processEvents();
        const QRect visibleFlush(flushFake->mapTo(scroll->viewport(), QPoint()), flushFake->size());
        QVERIFY2(scroll->viewport()->rect().contains(visibleFlush), "DNS cache actions must remain reachable by scrolling");
        QVERIFY(render(&page, "bottom"));
    }

    void trafficWheelScrollsHomePage() {
        core::MihomoClient client;
        ui::HomePage page(&client);
        page.resize(700, 500);
        page.show();
        auto *quick = page.windowHandle()->findChild<QQuickView *>("trafficGraphsView");
        auto *scroll = page.findChild<QScrollArea *>();
        QVERIFY(quick && scroll);
        QTRY_COMPARE(quick->status(), QQuickView::Ready);
        auto *bar = scroll->verticalScrollBar();
        QVERIFY(bar->maximum() > 0);
        const QPointF position(quick->width() / 2.0, quick->height() / 2.0);
        QWheelEvent wheel(position, quick->mapToGlobal(position), QPoint(), QPoint(0, -120),
                          Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(quick, &wheel);
        QTRY_VERIFY(bar->value() > 0);
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(HomePageTest)
#include "home_page_test.moc"
