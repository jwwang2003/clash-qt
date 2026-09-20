#pragma once

#include <QKeySequence>
#include <QObject>
#include <QString>

namespace platform {

/// System-wide hotkeys. Qt has no API for these, so each platform is wired to
/// its own native mechanism.
///
/// Contract with the ui module. Extend, do not reshape.
class Hotkeys : public QObject {
    Q_OBJECT

public:
    explicit Hotkeys(QObject *parent = nullptr);
    ~Hotkeys() override;

    static bool isSupported();

    /// Binds `sequence` to `id`, replacing any existing binding for that id.
    /// An empty sequence unbinds. False when the OS refused the combination,
    /// which usually means another app already owns it.
    bool bind(const QString &id, const QKeySequence &sequence);
    void unbind(const QString &id);
    void unbindAll();

    QString lastError() const;

signals:
    void triggered(const QString &id);
};

}  // namespace platform
