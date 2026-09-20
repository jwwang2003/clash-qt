#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>

#include "platform/proxy/system_proxy_service.h"

namespace {
using Action = platform::SystemProxyAction;
using Result = platform::SystemProxyResult;
using Config = platform::ProxyConfig;

Result confirmed(const Config &config = {}, bool owned = false) {
    Result result;
    result.state.supported = true;
    result.state.valid = true;
    result.state.config = config;
    result.state.owned = owned;
    return result;
}
}

class SystemProxyAsyncTest : public QObject {
    Q_OBJECT
private slots:
    void slowOperationDoesNotBlockGuiAndRefreshesDeduplicate() {
        struct Control { QSemaphore release; std::atomic_bool entered = false; std::atomic_bool workerThread = false; std::atomic_int calls = 0; };
        auto control = std::make_shared<Control>();
        platform::SystemProxyService service(nullptr, [control](Action, const Config &config) {
            ++control->calls;
            control->workerThread = QThread::currentThread() != QCoreApplication::instance()->thread();
            control->entered = true;
            control->release.tryAcquire(1, 3000);
            return confirmed(config, true);
        });
        QSignalSpy finished(&service, &platform::SystemProxyService::changeFinished);
        int heartbeats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(5);
        connect(&heartbeat, &QTimer::timeout, this, [&] { ++heartbeats; });
        heartbeat.start();
        QElapsedTimer elapsed;
        elapsed.start();
        service.setEnabled(true, {"127.0.0.1", 7890, "localhost", 7890});
        QVERIFY(elapsed.elapsed() < 100);
        QVERIFY(service.isBusy());
        QTRY_VERIFY(control->entered.load());
        for (int i = 0; i < 20; ++i) {
            service.refresh();
            service.setEnabled(true, {"127.0.0.1", 7890, "localhost", 7890});
        }
        QTest::qWait(45);
        QVERIFY(heartbeats >= 3);
        QCOMPARE(finished.size(), 0);
        QVERIFY(control->workerThread.load());
        control->release.release();
        QTRY_COMPARE(finished.size(), 1);
        QTRY_VERIFY(!service.isBusy());
        QCOMPARE(control->calls.load(), 1);
        QCOMPARE(service.state().config.port, quint16(7890));
    }

    void cachedReadDoesNotBlockAQueuedUserToggle() {
        auto gate = std::make_shared<QSemaphore>();
        auto reads = std::make_shared<std::atomic_int>(0);
        auto changes = std::make_shared<std::atomic_int>(0);
        platform::SystemProxyService service(nullptr, [gate, reads, changes](Action action, const Config &config) {
            if (action == Action::Refresh && ++*reads > 1) gate->tryAcquire(1, 3000);
            if (action == Action::Enable) ++*changes;
            return confirmed(config, action == Action::Enable);
        });
        service.refresh();
        QTRY_VERIFY(!service.isBusy());
        QVERIFY(service.state().valid);
        service.refresh();
        QTRY_COMPARE(reads->load(), 2);
        QVERIFY(service.isBusy());
        QVERIFY(!service.isChanging());
        service.setEnabled(true, {"127.0.0.1", 7890, {}, 7890});
        QVERIFY(service.isChanging());
        QCOMPARE(changes->load(), 0);
        gate->release();
        QTRY_VERIFY(!service.isBusy());
        QCOMPARE(changes->load(), 1);
        QVERIFY(!service.isChanging());
        QCOMPARE(service.state().config.port, quint16(7890));
    }

    void latestChangeWinsWithoutOverlappingWorkers() {
        struct Control { QSemaphore release; std::atomic_int active = 0; std::atomic_int maximum = 0; std::mutex mutex; QVector<QPair<Action, quint16>> actions; };
        auto control = std::make_shared<Control>();
        platform::SystemProxyService service(nullptr, [control](Action action, const Config &config) {
            const int active = ++control->active;
            control->maximum = qMax(control->maximum.load(), active);
            bool first;
            {
                const std::lock_guard lock(control->mutex);
                first = control->actions.isEmpty();
                control->actions.append({action, config.port});
            }
            if (first) control->release.tryAcquire(1, 3000);
            --control->active;
            return confirmed(config, action == Action::Enable);
        });
        QSignalSpy finished(&service, &platform::SystemProxyService::changeFinished);
        service.setEnabled(true, {"127.0.0.1", 7890, {}, 7890});
        QTRY_VERIFY(control->active.load() == 1);
        service.setEnabled(false);
        service.setEnabled(true, {"127.0.0.1", 7891, {}, 7891});
        service.refresh();
        control->release.release();
        QTRY_COMPARE(finished.size(), 2);
        QTRY_VERIFY(!service.isBusy());
        QCOMPARE(control->maximum.load(), 1);
        const std::lock_guard lock(control->mutex);
        QCOMPARE(control->actions.size(), 2);
        QCOMPARE(control->actions[0].first, Action::Enable);
        QCOMPARE(control->actions[0].second, quint16(7890));
        QCOMPARE(control->actions[1].first, Action::Enable);
        QCOMPARE(control->actions[1].second, quint16(7891));
        QCOMPARE(service.state().config.port, quint16(7891));
    }

    void failedOperationReportsConfirmedStateAndWorkerError() {
        bool unreadable = false;
        platform::SystemProxyService service(nullptr, [&unreadable](Action action, const Config &) {
            if (unreadable) {
                Result result;
                result.success = false;
                result.state.supported = true;
                result.state.error = result.error = "readback unavailable";
                return result;
            }
            Result result = confirmed({"existing-proxy", 8118, {}, 0});
            if (action == Action::Enable) { result.success = false; result.error = "permission denied"; }
            return result;
        });
        service.refresh();
        QTRY_VERIFY(!service.isBusy());
        QSignalSpy finished(&service, &platform::SystemProxyService::changeFinished);
        service.setEnabled(true, {"127.0.0.1", 7890, {}, 7890});
        QTRY_COMPARE(finished.size(), 1);
        QCOMPARE(finished.first().at(0).toBool(), true);
        QCOMPARE(finished.first().at(1).toBool(), false);
        QCOMPARE(finished.first().at(2).toString(), QString("permission denied"));
        QCOMPARE(service.state().config.host, QString("existing-proxy"));
        unreadable = true;
        service.refresh();
        QTRY_VERIFY(!service.isBusy());
        QVERIFY(!service.state().valid);
        QCOMPARE(service.state().config.port, quint16(8118));
        QCOMPARE(service.state().error, QString("readback unavailable"));
    }

    void shutdownDropsQueuedChangesAndRestoresLast() {
        struct Control { QSemaphore release; std::atomic_bool entered = false; std::mutex mutex; QVector<Action> actions; };
        auto control = std::make_shared<Control>();
        platform::SystemProxyService service(nullptr, [control](Action action, const Config &config) {
            {
                const std::lock_guard lock(control->mutex);
                control->actions.append(action);
            }
            if (action == Action::Enable) {
                control->entered = true;
                control->release.tryAcquire(1, 3000);
                return confirmed(config, true);
            }
            return confirmed();
        });
        QSignalSpy shutdown(&service, &platform::SystemProxyService::shutdownFinished);
        service.setEnabled(true, {"127.0.0.1", 7890, {}, 7890});
        QTRY_VERIFY(control->entered.load());
        service.setEnabled(true, {"127.0.0.1", 7891, {}, 7891});
        service.shutdown();
        QVERIFY(service.isShuttingDown());
        service.setEnabled(true, {"127.0.0.1", 7892, {}, 7892});
        service.refresh();
        QCOMPARE(shutdown.size(), 0);
        control->release.release();
        QTRY_COMPARE(shutdown.size(), 1);
        QVERIFY(shutdown.first().first().toBool());
        QVERIFY(!service.isBusy());
        QCOMPARE(service.state().config.port, quint16(0));
        service.setEnabled(true, {"127.0.0.1", 7893, {}, 7893});
        QTest::qWait(20);
        const std::lock_guard lock(control->mutex);
        QCOMPARE(control->actions, (QVector<Action>{Action::Enable, Action::Restore}));
    }

    void shutdownDuringRefreshNeverRunsQueuedEnable() {
        auto gate = std::make_shared<QSemaphore>();
        auto calls = std::make_shared<std::atomic_int>(0);
        platform::SystemProxyService service(nullptr, [gate, calls](Action action, const Config &) {
            ++*calls;
            if (action == Action::Refresh) gate->tryAcquire(1, 3000);
            Result result = confirmed();
            if (action == Action::Restore) { result.success = false; result.error = "restore failed"; }
            return result;
        });
        QSignalSpy shutdown(&service, &platform::SystemProxyService::shutdownFinished);
        service.refresh();
        service.setEnabled(true, {"127.0.0.1", 7890, {}, 7890});
        service.shutdown();
        gate->release();
        QTRY_COMPARE(shutdown.size(), 1);
        QCOMPARE(calls->load(), 2);
        QCOMPARE(shutdown.first().first().toBool(), false);
        QCOMPARE(shutdown.first().at(1).toString(), QString("restore failed"));
        QVERIFY(!service.isBusy());
    }

    void destroyingReceiverDoesNotWaitForWorker() {
        struct Control { QSemaphore release; std::atomic_bool entered = false; std::atomic_bool done = false; };
        auto control = std::make_shared<Control>();
        auto *service = new platform::SystemProxyService(nullptr, [control](Action, const Config &) {
            control->entered = true;
            control->release.tryAcquire(1, 3000);
            control->done = true;
            return confirmed();
        });
        service->refresh();
        QTRY_VERIFY(control->entered.load());
        QElapsedTimer elapsed;
        elapsed.start();
        delete service;
        QVERIFY(elapsed.elapsed() < 100);
        control->release.release();
        QTRY_VERIFY(control->done.load());
    }

    void workerExceptionBecomesFailureAndCanRetry() {
        auto count = std::make_shared<std::atomic_int>(0);
        platform::SystemProxyService service(nullptr, [count](Action, const Config &config) {
            if (++*count == 1) throw std::runtime_error("mock worker exception");
            return confirmed(config, true);
        });
        QSignalSpy finished(&service, &platform::SystemProxyService::changeFinished);
        service.setEnabled(true, {"127.0.0.1", 7890, {}, 7890});
        QTRY_COMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(1).toBool());
        QVERIFY(finished.first().at(2).toString().contains("mock worker exception"));
        QVERIFY(!service.isBusy());
        service.setEnabled(true, {"127.0.0.1", 7890, {}, 7890});
        QTRY_COMPARE(finished.size(), 2);
        QVERIFY(finished.last().at(1).toBool());
    }
};

QTEST_GUILESS_MAIN(SystemProxyAsyncTest)
#include "system_proxy_async_test.moc"
