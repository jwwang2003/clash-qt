// Frame-pacing measurement on a real display: records frameSwapped intervals for
// five seconds and reports screen, refresh rate, FPS, mean, median and p95. Its
// only assertion is that frames were produced at all; a pacing assertion is
// deliberately omitted, because display modes, ProMotion and power policy vary.
//
// Moved here from the former `data-pages` suite (tests/ui/data_pages_test.cpp),
// where it was registered in the routine lane and reported "Passed" on every run
// while in fact skipping: CTest never sets CLASH_QT_MEASURE_FRAMES and the lane is
// offscreen. Its CTest entry now carries the `benchmark` label, which the
// `make test` lane excludes, so it is honestly *not run* rather than counted as a
// pass. The gate below is unchanged, so an explicit run still skips for the same
// reason when the flag or the display is missing.
//
// It still does not record the workload, hardware, warm-up, duration and
// percentile metadata a benchmark is required to record -- it prints some of that
// and stores none of it. That gap is unchanged by this move.
#include <QtTest>
#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMutex>
#include <QQuickView>
#include <QQuickWindow>
#include <QScreen>
#include <QTimer>
#include <algorithm>
#include <memory>

#include "ui/pages/overview/traffic_graph.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class TrafficFramePacingTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("pacing"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
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

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(TrafficFramePacingTest)
#include "traffic_frame_pacing_test.moc"
