// Traffic graph widget: the series animates and scrolls between samples, pausing
// or hiding stops frames, and the embedded native QQuickView tracks the container
// geometry, hover inspection, focus order, the time-window axis and page
// visibility -- and is destroyed with the widget.
//
// Partition of the former `data-pages` suite (tests/ui/data_pages_test.cpp). Cases
// are carried over verbatim; see tests/README.md for the full original-to-new map.
#include <QtTest>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineSeries>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QQuickItem>
#include <QQuickView>
#include <QStackedWidget>
#include <QValueAxis>
#include <memory>

#include "ui/pages/overview/traffic_graph.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class TrafficGraphTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("graph"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
    }

    void trafficScrollsBetweenSamplesAndStopsWhenPausedOrHidden() {
        ui::TrafficGraph graph;
        graph.resize(700, 340);
        graph.show();
        auto *quick = graph.windowHandle()->findChild<QQuickView *>("trafficGraphsView");
        QVERIFY(quick);
        QTRY_COMPARE(quick->status(), QQuickView::Ready);
        graph.append(1000, 2000);
        QTest::qWait(25);
        graph.append(1200, 2400);
        auto *line = graph.findChild<QLineSeries *>("trafficDownloadSamples");
        auto *pause = graph.findChild<QPushButton *>("trafficPause");
        QVERIFY(line && pause);
        QTRY_COMPARE(line->count(), 2);
        QSignalSpy frames(line, &QLineSeries::pointsReplaced);
        // Native windows need exposure and pipeline startup before steady frames.
        QTRY_VERIFY(frames.count() >= 3);
        frames.clear();
        const auto initial = line->points();
        QTest::qWait(180);
        // More than a statistics timer tick: traffic must move between samples.
        QVERIFY2(frames.count() >= 3, "Traffic does not animate between incoming samples");
        QCOMPARE(line->count(), initial.size());
        QVERIFY(line->points().last().x() < initial.last().x() - 0.1);
        QCOMPARE(line->points().last().y(), initial.last().y());
        pause->click();
        const auto frozen = line->points();
        frames.clear();
        graph.append(9999, 9999);
        QTest::qWait(180);
        QCOMPARE(line->points(), frozen);
        QCOMPARE(frames.count(), 0);
        QVERIFY(!quick->rootObject()->property("animate").toBool());
        pause->click();
        QVERIFY(quick->rootObject()->property("animate").toBool());
        graph.hide();
        frames.clear();
        graph.append(20000, 20000);
        QTest::qWait(300);
        QCOMPARE(frames.count(), 0);
        QVERIFY(!quick->rootObject()->property("animate").toBool());
        graph.show();
        QTRY_VERIFY(line->count() > frozen.size());
        QVERIFY(quick->rootObject()->property("animate").toBool());
        graph.clear();
        QCOMPARE(line->count(), 0);
        QVERIFY(!quick->rootObject()->property("animate").toBool());
    }

    void trafficNativeWindowFollowsLayoutAndPageVisibility() {
        QStackedWidget pages;
        auto *graph = new ui::TrafficGraph;
        pages.addWidget(graph);
        pages.addWidget(new QWidget);
        pages.resize(700, 380);
        pages.show();
        auto *quick = pages.windowHandle()->findChild<QQuickView *>("trafficGraphsView");
        auto *container = graph->findChild<QWidget *>("trafficGraphsContainer");
        QVERIFY(quick && container);
        QPointer<QQuickView> lifetime(quick);
        QTRY_COMPARE(quick->status(), QQuickView::Ready);
        QSignalSpy errors(quick, &QQuickWindow::sceneGraphError);
        QTRY_COMPARE(quick->size(), container->size());
        pages.resize(850, 480);
        QTRY_COMPARE(quick->size(), container->size());
        QTRY_COMPARE(quick->rootObject()->size(), QSizeF(container->size()));
        QCOMPARE(container->focusPolicy(), Qt::NoFocus);
        graph->append(1000, 2000);
        QTest::qWait(200);
        graph->append(1200, 2400);
        auto *inspection = graph->findChild<QLabel *>("trafficInspection");
        auto *plot = quick->rootObject()->findChild<QQuickItem *>("trafficGraphsPlot");
        QVERIFY(inspection && plot);
        const QRectF plotArea = plot->property("plotArea").toRectF();
        QVERIFY(!plotArea.isEmpty());
        QTest::mouseMove(quick, QPoint(qRound(plotArea.right() - 1), qRound(plotArea.center().y())));
        QTRY_VERIFY(inspection->text().contains("ago"));
        QTest::mouseMove(quick, QPoint(1, 1));
        QTRY_VERIFY(inspection->text().contains("Hover to inspect"));
        auto *windowBox = graph->findChild<QComboBox *>("trafficTimeWindow");
        QVERIFY(windowBox);
        pages.activateWindow();
        windowBox->setFocus();
        QTest::keyClick(windowBox, Qt::Key_Tab);
        QTRY_VERIFY(graph->findChild<QPushButton *>("trafficPause")->hasFocus());
        windowBox->showPopup();
        auto *menu = windowBox->findChild<QMenu *>("comboPopupMenu");
        QVERIFY(menu);
        QTRY_VERIFY(menu->isVisible());
        menu->actions().at(1)->trigger();
        windowBox->hidePopup();
        QCOMPARE(windowBox->currentIndex(), 1);
        QCOMPARE(graph->findChild<QValueAxis *>("trafficTimeAxis")->min(), -300.0);
        pages.setCurrentIndex(1);
        QTRY_VERIFY(!quick->isVisible());
        QVERIFY(!quick->rootObject()->property("animate").toBool());
        pages.setCurrentIndex(0);
        QTRY_VERIFY(quick->isVisible());
        QVERIFY(quick->rootObject()->property("animate").toBool());
        QCOMPARE(errors.count(), 0);
        delete graph;
        QVERIFY(lifetime.isNull());
    }

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(TrafficGraphTest)
#include "traffic_graph_test.moc"
