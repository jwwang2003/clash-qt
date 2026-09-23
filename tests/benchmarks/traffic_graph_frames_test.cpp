// Rendering check on a real display: over 60 grabbed frames no coloured fill may
// appear above the baseline during zero-traffic runs, and the inspection label
// must still report zero rates for those samples -- the rendering workaround must
// not fabricate traffic.
//
// Moved here from the former `data-pages` suite (tests/ui/data_pages_test.cpp),
// where it was registered in the routine lane and reported "Passed" on every run
// while in fact skipping: CTest never sets CLASH_QT_VERIFY_GRAPH_FRAMES and the
// lane is offscreen. Its CTest entry now carries the `benchmark` label, which the
// `make test` lane excludes, so it is honestly *not run* rather than counted as a
// pass. The gate below is unchanged, so an explicit run still skips for the same
// reason when the flag or the display is missing.
//
// NOTE for whoever owns the native lane: this is a *correctness* test, not a
// measurement. tests/benchmarks/ is its interim home because the `native` lane has
// no directory yet; its entry is labelled `native` as well as `benchmark`, and it
// should move once that lane exists. Filing a rendering regression under a
// performance lane is not where it belongs.
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QImage>
#include <QLabel>
#include <QLineSeries>
#include <QQuickItem>
#include <QQuickView>
#include <QTimer>
#include <memory>

#include "ui/pages/overview/traffic_graph.h"
#include "ui/theme/theme.h"
#include "support/preference_isolation.h"
#include "support/scoped_environment.h"

class TrafficGraphFramesTest : public QObject {
    Q_OBJECT
private slots:
    // Preference isolation is per-executable, not per-case: QSettings redirection
    // and core::preferences are process-global, so every partition of the former
    // data-pages suite repeats this. The checks themselves live in
    // tests/support/preference_isolation.h so the copies cannot drift.
    void initTestCase() {
        environment_ = std::make_unique<testsupport::ScopedEnvironment>(QStringLiteral("frames"));
        const QString failure = testsupport::preferenceIsolationFailure(*environment_);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
    }
    void cleanupTestCase() {
        const QString escaped = testsupport::preferenceEscapeFailure(*environment_);
        QVERIFY2(escaped.isEmpty(), qPrintable(escaped));
        environment_.reset();
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

private:
    std::unique_ptr<testsupport::ScopedEnvironment> environment_;
};

QTEST_MAIN(TrafficGraphFramesTest)
#include "traffic_graph_frames_test.moc"
