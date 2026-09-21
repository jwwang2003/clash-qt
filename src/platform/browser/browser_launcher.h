#pragma once

#include <QString>
#include <QVector>

class QUrl;

namespace platform {

struct Browser {
    QString id;    // stable handle: bundle id, executable path, or desktop file
    QString name;  // display name
    bool isDefault = false;
};

/// Enumerates installed browsers and opens URLs in a chosen one.
///
/// Contract with the ui module. Extend, do not reshape.
class BrowserLauncher {
public:
    /// Installed browsers, the system default first. May be empty; open() still
    /// works in that case by deferring to the desktop's own handler.
    static QVector<Browser> available();

    /// Opens `url` in the browser identified by `browserId`. An empty id uses
    /// the system default.
    static bool open(const QUrl &url, const QString &browserId = QString());
};

}  // namespace platform
