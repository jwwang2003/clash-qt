#include "core/traffic_history.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace core {
namespace {

double boundedRate(double value) {
    if (!std::isfinite(value) || value < 0) return 0;
    return std::min(value, static_cast<double>(std::numeric_limits<quint64>::max()));
}

}  // namespace

std::optional<TrafficPoint> TrafficSnapshot::nearest(qint64 timestampMs) const {
    if (samples.isEmpty()) return std::nullopt;
    const auto after = std::lower_bound(samples.begin(), samples.end(), timestampMs,
        [](const TrafficPoint &point, qint64 timestamp) { return point.timestampMs < timestamp; });
    if (after == samples.begin()) return *after;
    if (after == samples.end()) return samples.last();
    const auto before = after - 1;
    return timestampMs - before->timestampMs <= after->timestampMs - timestampMs ? *before : *after;
}

bool TrafficHistory::append(qint64 timestampMs, double uploadBps, double downloadBps) {
    if (timestampMs < 0 || (!samples_.isEmpty() && timestampMs < samples_.last().timestampMs))
        return false;
    const TrafficPoint point{timestampMs, boundedRate(uploadBps), boundedRate(downloadBps)};
    if (!samples_.isEmpty() && timestampMs == samples_.last().timestampMs) {
        samples_.last() = point;
        return true;
    }
    samples_.append(point);
    const qint64 cutoff = timestampMs - FifteenMinutes;
    const auto first = std::lower_bound(samples_.begin(), samples_.end(), cutoff,
        [](const TrafficPoint &sample, qint64 timestamp) { return sample.timestampMs < timestamp; });
    const qsizetype expired = first - samples_.begin();
    const qsizetype overflow = std::max<qsizetype>(0, samples_.size() - MaxSamples);
    const qsizetype removeCount = std::max(expired, overflow);
    if (removeCount > 0) samples_.remove(0, removeCount);
    return true;
}

void TrafficHistory::clear() { samples_.clear(); }
qsizetype TrafficHistory::size() const { return samples_.size(); }
qint64 TrafficHistory::latestTimestampMs() const {
    return samples_.isEmpty() ? -1 : samples_.last().timestampMs;
}

TrafficSnapshot TrafficHistory::snapshot(qint64 windowMs, qint64 referenceMs) const {
    TrafficSnapshot result;
    result.endMs = std::max<qint64>(0, referenceMs);
    result.startMs = result.endMs - std::clamp(windowMs, OneMinute, FifteenMinutes);
    const auto first = std::lower_bound(samples_.begin(), samples_.end(), result.startMs,
        [](const TrafficPoint &point, qint64 timestamp) { return point.timestampMs < timestamp; });
    for (auto it = first; it != samples_.end() && it->timestampMs <= result.endMs; ++it) {
        result.samples.append(*it);
        auto &stats = result.stats;
        ++stats.sampleCount;
        stats.averageUploadBps += (it->uploadBps - stats.averageUploadBps) / stats.sampleCount;
        stats.averageDownloadBps += (it->downloadBps - stats.averageDownloadBps) / stats.sampleCount;
        stats.peakUploadBps = std::max(stats.peakUploadBps, it->uploadBps);
        stats.peakDownloadBps = std::max(stats.peakDownloadBps, it->downloadBps);
    }
    result.scale = scaleForPeak(std::max(result.stats.peakUploadBps, result.stats.peakDownloadBps));
    return result;
}

TrafficScale TrafficHistory::scaleForPeak(double bytesPerSecond) {
    TrafficScale result;
    const double peak = boundedRate(bytesPerSecond);
    if (peak == 0) return result;
    static constexpr const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s", "TiB/s", "PiB/s", "EiB/s"};
    int index = 0;
    while (peak / result.divisor >= 1024 && index < 6) {
        result.divisor *= 1024;
        ++index;
    }
    result.unit = QString::fromLatin1(units[index]);
    const double desired = std::max(1.0, peak / result.divisor * 1.1);
    const double magnitude = std::pow(10.0, std::floor(std::log10(desired)));
    const double fraction = desired / magnitude;
    const double nice = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10;
    result.maximum = nice * magnitude;
    return result;
}

}  // namespace core
