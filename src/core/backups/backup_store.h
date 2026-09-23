#pragma once

#include <QObject>
#include <QMap>
#include <QVariant>
#include <functional>
#include <memory>

class QNetworkAccessManager;

namespace core {

struct BackupOperation;
class BackupWorker;

class BackupStore : public QObject {
    Q_OBJECT
public:
    explicit BackupStore(const QString &dataDir, QObject *parent = nullptr);
    ~BackupStore() override;
    bool isBusy() const { return localBusy_ || transferring_; }
    QStringList localBackups() const;
    bool createLocal();
    bool exportArchive(const QString &destination);
    bool importArchive(const QString &source);
    bool restoreLocal(const QString &source);
    void createLocalAsync();
    void exportArchiveAsync(const QString &destination);
    void importArchiveAsync(const QString &source);
    void restoreLocalAsync(const QString &source);
    void continueRestore(bool approved);
    void requirePreparation(bool required) { preparationRequired_ = required; }
    void continuePreparation();
    void cancelAsync();
    void uploadWebDav(const QString &url, const QString &username, const QString &password);
    void downloadWebDav(const QString &url, const QString &username, const QString &password);

signals:
    void backupsChanged();
    void aboutToRestore();
    void restored();
    void errorOccurred(const QString &message);
    void statusChanged(const QString &message);
    void busyChanged(bool busy);
    void localBusyChanged(bool busy, bool restoring);
    void localOperationFinished(bool success);
    void restorePrepared();
    void operationPreparing();

private:
    friend class BackupWorker;
    QByteArray snapshot(QString *error) const;
    bool validate(const QByteArray &archive, QMap<QString, QByteArray> *files,
                  QMap<QString, QVariant> *settings, QString *error) const;
    bool saveLocal(const QByteArray &archive);
    void transfer(const QString &url, const QString &username, const QString &password, bool upload,
                  const QByteArray &body = {});
    void runAsync(std::function<bool(BackupStore &)> work,
                  std::function<void(bool)> completion = {}, bool restoring = false);

    QString dataDir_;
    QNetworkAccessManager *network_;
    bool transferring_ = false;
    bool localBusy_ = false;
    bool preparationRequired_ = false;
    std::shared_ptr<BackupOperation> operation_;
    std::function<bool()> approveRestore_;
};

}  // namespace core
