#pragma once

#include <QString>

class QDateTime;

namespace ui {

QString formatRate(quint64 bytesPerSecond);
QString formatBytes(quint64 bytes);
QString formatDelay(int delay);
QString formatDuration(const QDateTime &since);

/// A subscription URL carries an access token, so only its host is safe to
/// paint where a screenshot or a screen-share would pick it up.
QString redactUrl(const QString &url);

}  // namespace ui
