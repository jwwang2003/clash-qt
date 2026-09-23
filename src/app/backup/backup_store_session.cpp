#include "app/backup/backup_store_session.h"

#include <utility>

#include "core/backups/backup_store.h"

namespace app::backup {

BackupStoreSession::BackupStoreSession(const QString &dataDir, QObject *parent)
    : QObject(parent), store_(new core::BackupStore(dataDir, this)) {}

BackupStoreSession::~BackupStoreSession() = default;

void BackupStoreSession::setEvents(Events events) {
    events_ = std::move(events);

    // Connected once, to this session, and never to a widget. Every one of
    // these was a connection made from ui/backup_page.cpp or from main.cpp
    // against a store found by walking the widget tree.
    connect(store_, &core::BackupStore::busyChanged, this, [this](bool busy) {
        if (events_.busyChanged) events_.busyChanged(busy);
    });
    connect(store_, &core::BackupStore::localBusyChanged, this, [this](bool busy, bool restoring) {
        if (events_.localBusyChanged) events_.localBusyChanged(busy, restoring);
    });
    connect(store_, &core::BackupStore::operationPreparing, this, [this] {
        if (events_.operationPreparing) events_.operationPreparing();
    });
    connect(store_, &core::BackupStore::restorePrepared, this, [this] {
        if (events_.restorePrepared) events_.restorePrepared();
    });
    connect(store_, &core::BackupStore::restored, this, [this] {
        if (events_.restored) events_.restored();
    });
}

bool BackupStoreSession::isBusy() const { return store_->isBusy(); }

void BackupStoreSession::cancel() { store_->cancelAsync(); }

void BackupStoreSession::requirePreparation(bool required) { store_->requirePreparation(required); }

void BackupStoreSession::continuePreparation() { store_->continuePreparation(); }

void BackupStoreSession::continueRestore(bool approved) { store_->continueRestore(approved); }

}  // namespace app::backup
