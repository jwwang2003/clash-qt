#include <QtTest>

#include <cmath>
#include <limits>

#include "core/traffic_history.h"

class TrafficHistoryTest : public QObject {
    Q_OBJECT
private slots:
    void noSyntheticPrefixOrGapSamples() {
        core::TrafficHistory history;
        QVERIFY(history.append(50000, 100, 500));
        QVERIFY(history.append(55000, 300, 1500));
        const auto view = history.snapshot(core::TrafficHistory::OneMinute, 60000);
        QCOMPARE(view.startMs, qint64(0));
        QCOMPARE(view.endMs, qint64(60000));
        QCOMPARE(view.samples.size(), qsizetype(2));
        QCOMPARE(view.samples.first().timestampMs, qint64(50000));
        QCOMPARE(view.samples.last().timestampMs, qint64(55000));
        QCOMPARE(view.stats.sampleCount, qsizetype(2));
        QCOMPARE(view.stats.averageUploadBps, 200.0);
        QCOMPARE(view.stats.averageDownloadBps, 1000.0);
        QCOMPARE(view.stats.peakUploadBps, 300.0);
        QCOMPARE(view.stats.peakDownloadBps, 1500.0);
        const auto stale = history.snapshot(core::TrafficHistory::OneMinute, 200000);
        QVERIFY(stale.samples.isEmpty());
        QCOMPARE(stale.stats.averageUploadBps, 0.0);
        QVERIFY(!stale.nearest(200000));
    }
    void monotonicOrderingDeduplicationAndClamping() {
        core::TrafficHistory history;
        QVERIFY(!history.append(-1, 1, 2));
        QVERIFY(history.append(1000, 100, 200));
        QVERIFY(!history.append(999, 900, 900));
        QVERIFY(history.append(1000, 300, 400));
        QCOMPARE(history.size(), qsizetype(1));
        QVERIFY(history.append(2000, -1, std::numeric_limits<double>::quiet_NaN()));
        QVERIFY(history.append(3000, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::max()));
        const auto view = history.snapshot(core::TrafficHistory::OneMinute, 3000);
        QCOMPARE(view.samples.first().uploadBps, 300.0);
        QCOMPARE(view.samples[1].uploadBps, 0.0);
        QCOMPARE(view.samples[1].downloadBps, 0.0);
        QCOMPARE(view.samples[2].uploadBps, 0.0);
        QCOMPARE(view.samples[2].downloadBps,
                 static_cast<double>(std::numeric_limits<quint64>::max()));
        QVERIFY(std::isfinite(view.stats.averageDownloadBps));
        QVERIFY(std::isfinite(view.scale.maximum));
    }
    void timeWindowsExcludeOutsidePeaks() {
        core::TrafficHistory history;
        history.append(0, 10000, 50000);
        history.append(240000, 400, 800);
        history.append(270000, 200, 600);
        history.append(300000, 100, 300);
        history.append(310000, 99999, 99999);
        const auto minute = history.snapshot(core::TrafficHistory::OneMinute, 300000);
        QCOMPARE(minute.samples.size(), qsizetype(3));
        QCOMPARE(minute.stats.peakUploadBps, 400.0);
        QCOMPARE(minute.stats.peakDownloadBps, 800.0);
        const auto fiveMinutes = history.snapshot(core::TrafficHistory::FiveMinutes, 300000);
        QCOMPARE(fiveMinutes.samples.size(), qsizetype(4));
        QCOMPARE(fiveMinutes.stats.peakUploadBps, 10000.0);
        QCOMPARE(history.snapshot(0, 300000).startMs, qint64(240000));
        QCOMPARE(history.snapshot(std::numeric_limits<qint64>::max(), 300000).startMs, qint64(-600000));
        QCOMPARE(history.snapshot(core::TrafficHistory::OneMinute, -50).endMs, qint64(0));
    }
    void retentionIsBoundedByAgeAndCount() {
        core::TrafficHistory history;
        history.append(0, 100, 100);
        history.append(core::TrafficHistory::FifteenMinutes, 200, 200);
        QCOMPARE(history.size(), qsizetype(2));
        history.append(core::TrafficHistory::FifteenMinutes + 1, 300, 300);
        QCOMPARE(history.size(), qsizetype(2));
        history.clear();
        for (qsizetype i = 0; i < core::TrafficHistory::MaxSamples + 20; ++i)
            history.append(i, i, i);
        QCOMPARE(history.size(), core::TrafficHistory::MaxSamples);
        const auto view = history.snapshot(core::TrafficHistory::FifteenMinutes, history.latestTimestampMs());
        QCOMPARE(view.samples.first().timestampMs, qint64(20));
        QCOMPARE(view.samples.last().timestampMs, qint64(core::TrafficHistory::MaxSamples + 19));
    }
    void nearestObservedSampleUsesEarlierTie() {
        core::TrafficHistory history;
        history.append(1000, 100, 200);
        history.append(3000, 300, 400);
        const auto view = history.snapshot(core::TrafficHistory::OneMinute, 3000);
        QCOMPARE(view.nearest(-100)->timestampMs, qint64(1000));
        QCOMPARE(view.nearest(2000)->timestampMs, qint64(1000));
        QCOMPARE(view.nearest(2001)->timestampMs, qint64(3000));
        QCOMPARE(view.nearest(3000)->timestampMs, qint64(3000));
        QCOMPARE(view.nearest(9000)->timestampMs, qint64(3000));
        QVERIFY(!core::TrafficSnapshot{}.nearest(1000));
    }
    void pausedSnapshotSurvivesContinuedCaptureAndEviction() {
        core::TrafficHistory history;
        history.append(0, 1000, 2000);
        history.append(1000, 100, 200);
        const core::TrafficHistory pausedHistory = history;
        const auto paused = history.snapshot(core::TrafficHistory::OneMinute, 1000);
        history.append(core::TrafficHistory::FifteenMinutes + 2000, 1, 2);
        const auto live = history.snapshot(core::TrafficHistory::OneMinute, history.latestTimestampMs());
        QCOMPARE(live.stats.peakUploadBps, 1.0);
        QCOMPARE(paused.stats.peakUploadBps, 1000.0);
        QCOMPARE(paused.endMs, qint64(1000));
        QCOMPARE(pausedHistory.snapshot(core::TrafficHistory::FiveMinutes, 1000).stats.peakUploadBps, 1000.0);
        history.clear();
        QCOMPARE(history.size(), qsizetype(0));
        QCOMPARE(history.latestTimestampMs(), qint64(-1));
        QVERIFY(history.snapshot(core::TrafficHistory::OneMinute, 0).samples.isEmpty());
        QVERIFY(history.append(0, 1, 2));
        QCOMPARE(paused.samples.size(), qsizetype(2));
    }
    void scalesUseIecUnitsAndNiceCeilings() {
        const auto empty = core::TrafficHistory::scaleForPeak(0);
        QCOMPARE(empty.unit, QString("B/s"));
        QCOMPARE(empty.maximum, 1.0);
        const auto kib = core::TrafficHistory::scaleForPeak(1024);
        QCOMPARE(kib.unit, QString("KiB/s"));
        QCOMPARE(kib.divisor, 1024.0);
        QCOMPARE(kib.maximum, 2.0);
        const auto mib = core::TrafficHistory::scaleForPeak(6 * 1024 * 1024);
        QCOMPARE(mib.unit, QString("MiB/s"));
        QCOMPARE(mib.divisor, 1024.0 * 1024.0);
        QCOMPARE(mib.maximum, 10.0);
        for (double peak : {0.25, 1.0, 1023.0, 1024.0, 5e8, 1e15, 1e19}) {
            const auto scale = core::TrafficHistory::scaleForPeak(peak);
            QVERIFY(scale.maximum >= peak / scale.divisor);
            QVERIFY(std::isfinite(scale.maximum));
            QVERIFY(scale.divisor >= 1);
        }
    }
};

QTEST_GUILESS_MAIN(TrafficHistoryTest)
#include "traffic_history_test.moc"
