#include "ui/theme/formatting.h"

#include <QDateTime>
#include <QUrl>

namespace ui {

QString formatRate(quint64 bytesPerSecond) {
    static const char *units[] = {"B/s", "KiB/s", "MiB/s", "GiB/s"};
    double value = static_cast<double>(bytesPerSecond);
    int unit = 0;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', value < 10.0 ? 1 : 0) + ' ' + units[unit];
}

QString formatBytes(quint64 bytes) {
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    return QString::number(value, 'f', value < 10.0 && unit > 0 ? 1 : 0) + ' ' + units[unit];
}

QString formatDelay(int delay) {
    if (delay < 0) return "—";
    if (delay == 0) return "timeout";
    return QString::number(delay) + " ms";
}

QString formatDuration(const QDateTime &since) {
    const qint64 seconds = qMax<qint64>(0, since.secsTo(QDateTime::currentDateTime()));
    if (seconds < 60) return QString("%1s").arg(seconds);
    if (seconds < 3600) return QString("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    return QString("%1h %2m").arg(seconds / 3600).arg(seconds % 3600 / 60);
}

QString redactUrl(const QString &url) {
    const QString host = QUrl(url).host();
    return host.isEmpty() ? QStringLiteral("hidden link") : host;
}

}  // namespace ui
