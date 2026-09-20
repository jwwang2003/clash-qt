#include <QtTest>
#include <QComboBox>
#include <QApplication>
#include <QClipboard>
#include <QDialog>
#include <QSettings>
#include <QTemporaryDir>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTableView>
#include <QQuickWidget>
#include <QQuickItem>
#include <QLineSeries>
#include <QValueAxis>
#include "ui/traffic_graph.h"
#include <QtGraphs/QAreaSeries>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QTcpServer>
#include <algorithm>
#include "ui/home_page.h"
#include "ui/settings_section.h"
#include "ui/theme.h"
#include "core/mihomo_client.h"
#include "ui/connections_page.h"
#include "ui/logs_page.h"
#include "ui/proxies_page.h"
#include "ui/rules_page.h"
#include "ui/providers_page.h"
#include "core/provider_client.h"

class DataPagesTest : public QObject {
    Q_OBJECT
    QTemporaryDir configDir_;
private slots:
    void initTestCase() {
        QVERIFY(configDir_.isValid());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, configDir_.path());
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
        auto *graphView = chart->findChild<QQuickWidget *>("trafficGraphsView");
        QVERIFY(graphView);
        QTRY_COMPARE(graphView->status(), QQuickWidget::Ready);
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
        auto *quick = graph.findChild<QQuickWidget *>("trafficGraphsView");
        QTRY_COMPARE(quick->status(), QQuickWidget::Ready);
        graph.append(1000, 2000);
        QTest::qWait(25);
        graph.append(1200, 2400);
        auto *line = graph.findChild<QLineSeries *>("trafficDownloadSamples");
        auto *pause = graph.findChild<QPushButton *>("trafficPause");
        QVERIFY(line && pause);
        QTRY_COMPARE(line->count(), 2);
        QSignalSpy frames(line, &QLineSeries::pointsReplaced);
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
