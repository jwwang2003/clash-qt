#include <QtTest>
#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QQuickView>
#include <QQuickItem>
#include <QScreen>
#include <QStackedWidget>
#include <QMenu>
#include <QMutex>
#include <QPointer>
#include <QPainter>
#include <QWheelEvent>
#include <QEventLoop>
#include <QTimer>
#include <memory>
#include <QLineSeries>
#include <QValueAxis>
#include "ui/pages/overview/traffic_graph.h"
#include <QtGraphs/QAreaSeries>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTcpServer>
#include <algorithm>
#include "ui/pages/overview/home_page.h"
#include "ui/widgets/settings_section.h"
#include "ui/theme/theme.h"
#include "core/mihomo/mihomo_client.h"
#include "ui/pages/connections/connections_page.h"
#include "ui/pages/logs/logs_page.h"
#include "ui/pages/proxies/proxies_page.h"
#include "ui/pages/rules/rules_page.h"
#include "ui/pages/providers/providers_page.h"
#include "core/mihomo/provider_client.h"
#include "core/preferences/preferences.h"
#include "support/scoped_environment.h"

class DataPagesTest : public QObject {
    Q_OBJECT
    // ProxiesPage persists "proxies/sort". QSettings::setDefaultFormat() and
    // setPath(), which used to stand here, cannot redirect
    // QSettings(organization, application) on macOS, so that write reached the
    // developer's real preferences. CLASH_QT_DATA_DIR, which core::preferences
    // honours, does isolate it.
    std::unique_ptr<testsupport::ScopedEnvironment> configDir_;
private slots:
    void initTestCase() {
        configDir_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("pages"));
        QVERIFY2(configDir_->isValid(), qPrintable(configDir_->errorString()));
        QVERIFY(core::preferences::isIsolated());
        QVERIFY2(core::preferences::fileName().startsWith(
                     QFileInfo(configDir_->dataDir()).absoluteFilePath() + QLatin1Char('/')),
                 qPrintable(core::preferences::fileName()));
    }
    void cleanupTestCase() {
        QVERIFY2(configDir_->realPreferencesUnchanged(),
                 qPrintable(QStringLiteral("The real user preference store at %1 changed during this run")
                                .arg(configDir_->productionSettingsFilePath())));
        configDir_.reset();
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

    void trafficNativeFillStaysBelowOutline() {
        if (!qEnvironmentVariableIsSet("CLASH_QT_VERIFY_GRAPH_FRAMES"))
            QSKIP("Set CLASH_QT_VERIFY_GRAPH_FRAMES=1 on a native GPU display");
        QVERIFY(QGuiApplication::platformName() != "offscreen");
        ui::theme::install();
        ui::TrafficGraph graph;
        graph.resize(900, 400);
        graph.show();
        graph.raise();
        graph.activateWindow();
        auto *quick = graph.windowHandle()->findChild<QQuickView *>("trafficGraphsView");
        QVERIFY(quick);
        QTRY_VERIFY(quick->isExposed());
        QTimer samples;
        samples.setInterval(500);
        graph.append(4000, 6000); // A nonzero first point followed by zero runs.
        int sample = 0;
        connect(&samples, &QTimer::timeout, &graph, [&] {
            const int phase = sample++ % 10;
            graph.append(phase == 7 ? 4000 : 0, phase == 7 ? 6000 : 0);
        });
        samples.start();
        QEventLoop seed;
        QTimer::singleShot(10000, &seed, &QEventLoop::quit);
        seed.exec();
        samples.stop();
        auto *plot = quick->rootObject()->findChild<QQuickItem *>("trafficGraphsPlot");
        QVERIFY(plot);
        auto *line = graph.findChild<QLineSeries *>("trafficDownloadSamples");
        QVERIFY(line);
        int invalidFrames = 0, checkedPixels = 0;
        const QString output = qEnvironmentVariable("CLASH_QT_AUDIT_IMAGES");
        if (!output.isEmpty()) QVERIFY(QDir().mkpath(output));
        for (int frame = 0; frame < 60; ++frame) {
            const QImage image = quick->grabWindow().convertToFormat(QImage::Format_RGB32);
            QVERIFY(!image.isNull());
            const qreal scale = image.width() / qreal(quick->width());
            const QRectF plotArea = plot->property("plotArea").toRectF();
            const auto points = line->points();
            bool invalid = false;
            for (qsizetype i = 1; i < points.size(); ++i) {
                // Both series have the same zero runs. Well above their baseline
                // there must be no colored fill, even as the graph scrolls.
                if (points[i - 1].y() > 1e-12 || points[i].y() > 1e-12) continue;
                const double seconds = (points[i - 1].x() + points[i].x()) / 2;
                const int x = qRound((plotArea.x() + plotArea.width() * (seconds + 60) / 60) * scale);
                const int y = qRound(plotArea.bottom() * scale) - 16;
                if (!image.rect().contains(QPoint(x, y))) continue;
                const QColor pixel = image.pixelColor(x, y);
                ++checkedPixels;
                if (pixel.green() > pixel.red() + 15 || pixel.blue() > pixel.red() + 20)
                    invalid = true;
            }
            if (invalid) ++invalidFrames;
            if (!output.isEmpty() && (frame == 0 || invalid))
                QVERIFY(image.save(QDir(output).filePath(invalid ? "traffic-invalid-fill.png" : "traffic-fill.png")));
            QTest::qWait(8);
        }
        qInfo() << "Frames with fill above zero traffic:" << invalidFrames << "/ 60; probes:" << checkedPixels;
        QVERIFY(checkedPixels > 100);
        QCOMPARE(invalidFrames, 0);
        // The rendering workaround must not turn measured zeros into fake rates.
        const auto points = line->points();
        bool inspectedZero = false;
        for (qsizetype i = 1; i < points.size(); ++i) {
            if (points[i - 1].y() > 1e-12 || points[i].y() > 1e-12) continue;
            const double fraction = (60 + (points[i - 1].x() + points[i].x()) / 2) / 60;
            QVERIFY(QMetaObject::invokeMethod(&graph, "showSampleAt", Q_ARG(double, fraction)));
            QVERIFY(graph.findChild<QLabel *>("trafficInspection")->text().contains("↓ 0.0 B/s · ↑ 0.0 B/s"));
            inspectedZero = true;
            break;
        }
        QVERIFY(inspectedZero);
    }

    void trafficNativeFrameTiming() {
        if (!qEnvironmentVariableIsSet("CLASH_QT_MEASURE_FRAMES"))
            QSKIP("Set CLASH_QT_MEASURE_FRAMES=1 on a native display to measure frame pacing");
        QVERIFY2(QGuiApplication::platformName() != "offscreen",
                 "Frame pacing must be measured on a native display, outside CTest");
        // Shared state also survives an in-flight render-thread callback at teardown.
        struct Timing {
            QMutex mutex;
            QElapsedTimer clock;
            QList<qint64> timestamps;
        };
        auto timing = std::make_shared<Timing>();
        ui::TrafficGraph graph;
        graph.resize(800, 380);
        graph.show();
        graph.raise();
        graph.activateWindow();
        auto *quick = graph.windowHandle()->findChild<QQuickView *>("trafficGraphsView");
        QVERIFY(quick);
        QTRY_VERIFY(quick->isExposed());
        graph.append(1000, 2000);
        QTest::qWait(100);
        graph.append(1200, 2400);
        QTest::qWait(500); // Warm up pipeline creation and the animation driver.
        timing->clock.start();
        const auto connection = connect(quick, &QQuickWindow::frameSwapped, &graph, [timing] {
            QMutexLocker lock(&timing->mutex);
            timing->timestamps.append(timing->clock.nsecsElapsed());
        }, Qt::DirectConnection);
        // A real event loop avoids QtTest's polling sleeps limiting GUI frames.
        QEventLoop measurement;
        QTimer::singleShot(5000, &measurement, &QEventLoop::quit);
        measurement.exec();
        disconnect(connection);
        QList<qint64> timestamps;
        {
            QMutexLocker lock(&timing->mutex);
            timestamps = timing->timestamps;
        }
        QVERIFY(timestamps.size() > 2);
        QList<double> intervals;
        for (qsizetype i = 1; i < timestamps.size(); ++i)
            intervals.append((timestamps[i] - timestamps[i - 1]) / 1e6);
        std::sort(intervals.begin(), intervals.end());
        const double mean = (timestamps.last() - timestamps.first()) / 1e6 / intervals.size();
        qInfo().nospace() << "Screen: " << quick->screen()->name()
            << ", reported refresh: " << quick->screen()->refreshRate()
            << " Hz, frameSwapped: " << 1000.0 / mean << " FPS, mean: " << mean
            << " ms, median: " << intervals[intervals.size() / 2]
            << " ms, p95: " << intervals[intervals.size() * 95 / 100] << " ms";
        // Deliberately no 120 FPS assertion: display modes, ProMotion, and power
        // policy vary. frameSwapped measures submissions, not physical scanout.
    }

    void emptyProxySnapshotClearsMembersAndAction() {
        core::MihomoClient client;
        ui::ProxiesPage page(&client);
        page.show();
        client.proxiesUpdated({core::ProxyGroup{"Choose", "Selector", "Node", {"Node"}}},
                              {{"Node", core::ProxyNode{"Node", "Direct", 20}}});
        const auto lists = page.findChildren<QListWidget *>();
        QCOMPARE(lists.size(), 2);
        QTRY_COMPARE(lists[0]->count(), 1);
        QCOMPARE(lists[1]->count(), 1);
        client.proxiesUpdated({}, {});
        QTRY_COMPARE(lists[0]->count(), 0);
        QCOMPARE(lists[1]->count(), 0);
        for (auto *button : page.findChildren<QPushButton *>())
            if (button->text() == "Test Latency") QVERIFY(!button->isEnabled());
    }

    void proxyFilterAndLatencySort() {
        core::MihomoClient client;
        ui::ProxiesPage page(&client);
        page.show();
        client.proxiesUpdated({core::ProxyGroup{"Choose", "Selector", "Fast", {"Slow", "Fast", "Untested"}}},
                              {{"Slow", core::ProxyNode{"Slow", "Direct", 200}},
                               {"Fast", core::ProxyNode{"Fast", "Direct", 10}},
                               {"Untested", core::ProxyNode{"Untested", "Direct", -1}}});
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
        core::MihomoClient client;
        ui::ProxiesPage page(&client);
        page.show();
        QVector<core::ProxyGroup> groups{{"Choose", "Selector", "Node", {"Node"}}};
        QHash<QString, core::ProxyNode> nodes{{"Node", {"Node", "Direct", 20}}};
        auto *list = page.findChild<QListWidget *>("proxyNodes");
        client.proxiesUpdated(groups, nodes);
        QTRY_COMPARE(list->count(), 1);
        QSignalSpy resets(list->model(), &QAbstractItemModel::modelReset);
        for (int i = 0; i < 10; ++i) client.proxiesUpdated(groups, nodes);
        QTest::qWait(30);
        QCOMPARE(resets.size(), 0);
        page.hide();
        groups[0].all << "Next";
        nodes.insert("Next", {"Next", "Direct", 30});
        client.proxiesUpdated(groups, nodes);
        QTest::qWait(30);
        QCOMPARE(resets.size(), 0);
        QCOMPARE(list->count(), 1);
        page.show();
        QTRY_COMPARE(list->count(), 2);
    }

    void hiddenConnectionsStillCaptureRatesAndClosedHistory() {
        core::MihomoClient client;
        ui::ConnectionsPage page(&client);
        page.show();
        core::Connection first;
        first.id = "first"; first.host = "first.example";
        client.connectionsUpdated({first}, 0, 0);
        auto *model = page.findChild<ui::ConnectionModel *>();
        QTRY_COMPARE(model->rowCount(), 1);
        QSignalSpy resets(model, &QAbstractItemModel::modelReset);
        page.hide();
        core::Connection second;
        second.id = "second"; second.host = "second.example";
        client.connectionsUpdated({second}, 0, 0);
        QTest::qWait(25);
        second.upload = 2000;
        client.connectionsUpdated({second}, 2000, 0);
        QTest::qWait(125);
        QCOMPARE(resets.size(), 0);
        page.show();
        QTRY_COMPARE(model->idAt(0), QString("second"));
        QVERIFY(model->index(0, ui::ConnectionModel::UploadRate).data(Qt::UserRole).toDouble() > 0);
        page.findChild<QComboBox *>()->setCurrentIndex(1);
        QCOMPARE(model->idAt(0), QString("first"));
        resets.clear();
        second.upload += 500;
        client.connectionsUpdated({second}, 2500, 0);
        QTest::qWait(125);
        QCOMPARE(resets.size(), 0);
    }

    void providersSkipHiddenAndUnchangedRebuilds() {
        QTcpServer fixture;
        QVERIFY(fixture.listen(QHostAddress::LocalHost));
        core::MihomoClient client;
        client.setEndpoint({"127.0.0.1", fixture.serverPort(), {}});
        ui::ProvidersPage page(&client);
        auto *backend = page.findChild<core::ProviderClient *>();
        auto *view = page.findChild<QTableView *>();
        QVERIFY(backend && view);
        core::Provider provider;
        provider.name = "Test provider";
        provider.vehicle = "HTTP";
        provider.count = 25;
        backend->providersReceived(false, {provider});
        QCOMPARE(view->model()->rowCount(), 0);
        page.show();
        QTRY_COMPARE(view->model()->rowCount(), 1);
        QSignalSpy resets(view->model(), &QAbstractItemModel::modelReset);
        backend->providersReceived(false, {provider});
        QTest::qWait(30);
        QCOMPARE(resets.size(), 0);
        page.hide();
        provider.count = 30;
        backend->providersReceived(false, {provider});
        QCOMPARE(view->model()->index(0, 3).data().toInt(), 25);
        page.show();
        QTRY_COMPARE(view->model()->index(0, 3).data().toInt(), 30);
    }

    void connectionSnapshotPreservesSelectedId() {
        core::MihomoClient client;
        ui::ConnectionsPage page(&client);
        page.show();
        core::Connection a, b;
        a.id = "a"; a.host = "a.example";
        b.id = "b"; b.host = "b.example";
        client.connectionsUpdated({a, b}, 0, 0);
        auto *view = page.findChild<QTableView *>();
        QTRY_COMPARE(view->model()->rowCount(), 2);
        view->sortByColumn(0, Qt::AscendingOrder);
        view->setCurrentIndex(view->model()->index(1, 0));
        QCOMPARE(view->currentIndex().data().toString(), QString("b.example"));
        client.connectionsUpdated({b, a}, 100, 200);
        QTRY_COMPARE(page.findChild<ui::ConnectionModel *>()->idAt(0), QString("b"));
        QCOMPARE(view->currentIndex().data().toString(), QString("b.example"));
    }

    void connectionRatesSortAndResetOnCounterRollback() {
        core::MihomoClient client;
        ui::ConnectionsPage page(&client);
        page.show();
        core::Connection slow, fast;
        slow.id = "slow"; slow.host = "slow.example";
        fast.id = "fast"; fast.host = "fast.example";
        client.connectionsUpdated({slow, fast}, 0, 0);
        QTest::qWait(25);
        slow.upload = 100; slow.download = 200;
        fast.upload = 10000; fast.download = 20000;
        client.connectionsUpdated({slow, fast}, 10100, 20200);
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
        client.connectionsUpdated({slow, fast}, 0, 0);
        QTRY_COMPARE(model->index(0, ui::ConnectionModel::UploadRate).data(Qt::UserRole).toDouble(), 0.0);
    }

    void closedHistoryIsBoundedAndHasFrozenDuration() {
        core::MihomoClient client;
        ui::ConnectionsPage page(&client);
        page.show();
        QVector<core::Connection> connections;
        for (int i = 0; i < 600; ++i) {
            core::Connection connection;
            connection.id = QString::number(i);
            connection.host = QString("host%1.example").arg(i);
            connection.start = QDateTime::currentDateTime().addSecs(-3600);
            connections.append(connection);
        }
        client.connectionsUpdated(connections, 0, 0);
        client.connectionsUpdated({}, 0, 0);
        page.findChild<QComboBox *>()->setCurrentIndex(1);
        auto *model = page.findChild<ui::ConnectionModel *>();
        QCOMPARE(model->rowCount(), 500);
        QVERIFY(model->connectionAt(0)->end.isValid());
        const auto duration = model->index(0, ui::ConnectionModel::Duration).data(Qt::UserRole);
        QTest::qWait(25);
        QCOMPARE(model->index(0, ui::ConnectionModel::Duration).data(Qt::UserRole), duration);
        client.endpointChanged();
        QCOMPARE(model->rowCount(), 0);
    }

    void connectionDetailsCanBeCopied() {
        core::MihomoClient client;
        ui::ConnectionsPage page(&client);
        page.show();
        core::Connection connection;
        connection.id = "detail-id";
        connection.host = "test.example";
        connection.sourceIp = "127.0.0.1";
        connection.sourcePort = "3210";
        connection.processPath = "/Applications/Test App.app/Contents/MacOS/test";
        connection.destinationPort = "443";
        client.connectionsUpdated({connection}, 0, 0);
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

    void logsFilterExistingHistoryAndEscapePayload() {
        core::MihomoClient client;
        ui::LogsPage page(&client);
        client.logReceived({"info", "<hello>", QDateTime::currentDateTime()});
        client.logReceived({"error", "failed", QDateTime::currentDateTime()});
        auto *view = page.findChild<QPlainTextEdit *>();
        QVERIFY(view->toPlainText().contains("<hello>"));
        auto *filter = page.findChild<QLineEdit *>();
        filter->setText("failed");
        QTRY_VERIFY(!view->toPlainText().contains("<hello>"));
        QVERIFY(view->toPlainText().contains("failed"));
        filter->clear();
        QTRY_VERIFY(view->toPlainText().contains("<hello>"));
        client.endpointChanged();
        QVERIFY(view->toPlainText().isEmpty());
        filter->setText("hello");
        QVERIFY(view->toPlainText().isEmpty());
        QVERIFY(view->font().pointSizeF() >= page.font().pointSizeF());
    }
};

QTEST_MAIN(DataPagesTest)
#include "data_pages_test.moc"
