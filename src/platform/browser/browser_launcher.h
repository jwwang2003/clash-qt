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

/// The two browser operations, as an interface a caller can be given instead of
/// the static functions below. Implementations may be called from a worker
/// thread, so they must not touch the GUI.
///
/// Contract with the ui module. Extend, do not reshape.
class BrowserOperations {
public:
    virtual ~BrowserOperations() = default;
    virtual QVector<Browser> available() = 0;
    virtual bool open(const QUrl &url, const QString &browserId) = 0;
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

    /// The production operations object: a stateless adapter over the two
    /// functions above. It lives for the whole process, so background work may
    /// keep the pointer after its caller is gone.
    static BrowserOperations *operations();
};

}  // namespace platform
