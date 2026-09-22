#ifndef CLASHQT_APP_BACKUP_BACKUP_STORE_SESSION_H
#define CLASHQT_APP_BACKUP_BACKUP_STORE_SESSION_H

// BackupStoreSession: the production BackupSession. It CONSTRUCTS AND OWNS the
// process's one core::BackupStore.
//
// This is what removes the widget-tree service discovery at main.cpp:182
//
//     const auto backups = window.findChildren<core::BackupStore *>();
//
// which found the store only because ui/backup_page.cpp:28 parents it to a page
// that MainWindow happens to build eagerly. A lazily constructed BackupPage
// would have returned an empty list and shutdown would have silently skipped
// both the backup cancel and the backup busy gate. Here
// the store exists because this object exists, and the page is handed one.

#include <QObject>
#include <QString>

#include "app/backup/backup_session.h"

namespace core {
class BackupStore;
}

namespace app::backup {

class BackupStoreSession final : public QObject, public BackupSession {
    Q_OBJECT

  public:
    // `dataDir` is core::ProfileStore::dataDir(), which is what
    // ui/backup_page.cpp:28 passed.
    explicit BackupStoreSession(const QString &dataDir, QObject *parent = nullptr);
    ~BackupStoreSession() override;

    void setEvents(Events events) override;

    bool isBusy() const override;
    void cancel() override;
    void requirePreparation(bool required) override;
    void continuePreparation() override;
    void continueRestore(bool approved) override;

    core::BackupStore *store() const noexcept override { return store_; }

  private:
    core::BackupStore *store_;
    Events events_;
};

}  // namespace app::backup

#endif  // CLASHQT_APP_BACKUP_BACKUP_STORE_SESSION_H
