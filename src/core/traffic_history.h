#pragma once

#include <QString>
#include <QVector>

#include <optional>

namespace core {

struct TrafficPoint {
    qint64 timestampMs = 0;
    double uploadBps = 0;
    double downloadBps = 0;
};

struct TrafficStats {
    qsizetype sampleCount = 0;
    double averageUploadBps = 0;
    double averageDownloadBps = 0;
    double peakUploadBps = 0;
    double peakDownloadBps = 0;
};

struct TrafficScale {
    QString unit = QStringLiteral("B/s");
    double divisor = 1;
    double maximum = 1;  // axis maximum after dividing byte rates by divisor
};

struct TrafficSnapshot {
    qint64 startMs = 0;
    qint64 endMs = 0;
    QVector<TrafficPoint> samples;
    TrafficStats stats;
    TrafficScale scale;

    /// Nearest observed sample, clamped to the observed endpoints. Ties use
    /// the earlier sample. An empty snapshot has no nearest sample.
    std::optional<TrafficPoint> nearest(qint64 timestampMs) const;
};

/// A value type with no clock or network ownership. A copied history plus a
/// frozen reference timestamp supports pause while live capture continues.
class TrafficHistory {
public:
    static constexpr qint64 OneMinute = 60 * 1000;
    static constexpr qint64 FiveMinutes = 5 * OneMinute;
    static constexpr qint64 FifteenMinutes = 15 * OneMinute;
    static constexpr qsizetype MaxSamples = 4096;

    /// Rejects negative or out-of-order timestamps. A repeated timestamp
    /// replaces its sample. Invalid/negative rates become zero; excessive
    /// rates are capped at the largest representable quint64 byte rate.
    bool append(qint64 timestampMs, double uploadBps, double downloadBps);
    void clear();
    qsizetype size() const;
    qint64 latestTimestampMs() const;  // -1 when empty

    /// Returns only observed samples in [referenceMs-windowMs, referenceMs].
    /// Windows are clamped to 1–15 minutes, and negative references to zero.
    /// Means use observed sample rates; missing intervals are never zero-filled
    /// or treated as sustained traffic. The returned snapshot owns its data.
    TrafficSnapshot snapshot(qint64 windowMs, qint64 referenceMs) const;
    static TrafficScale scaleForPeak(double bytesPerSecond);

private:
    QVector<TrafficPoint> samples_;
};

}  // namespace core
