#pragma once

#include <QDateTime>
#include <QObject>
#include <QJsonObject>
#include <QPointer>
#include <QHash>
#include <QQueue>
#include <QVector>
#include <atomic>
#include <memory>

#include "core/types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace core {

class ConfigEnhancer;

/// Traffic allowance reported by a subscription's `subscription-userinfo` header.
struct SubscriptionInfo {
    quint64 upload = 0;
    quint64 download = 0;
    quint64 total = 0;
    QDateTime expire;

    bool isEmpty() const { return total == 0 && !expire.isValid(); }
};

struct Profile {
    QString uid;
    QString name;
    bool remote = false;   // remote profiles have a url and can be updated
    QString url;
    QString filePath;      // absolute path to the profile's yaml
    QDateTime updated;
    int updateIntervalMinutes = 0;  // 0 disables automatic refresh
    SubscriptionInfo subscription;
};

/// Owns profiles on disk: the index, the downloaded yaml files, subscription
/// refresh, and the merged runtime config the core is launched with.
///
/// Contract with the ui module. Extend, do not reshape.
class ProfileStore : public QObject {
    Q_OBJECT

public:
    explicit ProfileStore(QObject *parent = nullptr);
    ~ProfileStore() override;

    /// Application data directory; profiles live in `<dataDir>/profiles`.
    QString dataDir() const;

    void load();
    void setMaintenanceMode(bool enabled);
    void beginShutdown();
    void setEnhancer(ConfigEnhancer *enhancer);
    QJsonObject runtimeOverrides() const;
    bool setRuntimeOverrides(const QJsonObject &overrides);
    QVector<Profile> profiles() const;
    QString currentUid() const;

    void selectProfile(const QString &uid);
    void importFromUrl(const QString &url, const QString &name = {});
    void importFromFile(const QString &path);
    bool createLocalProfile(const QString &name, const QString &yaml);
    void updateProfile(const QString &uid);
    bool setSubscriptionUrl(const QString &uid, const QString &url);
    bool saveProfileContent(const QString &uid, const QString &yaml);
    void importFromFileAsync(const QString &path);
    void createLocalProfileAsync(const QString &name, const QString &yaml);
    void saveProfileContentAsync(const QString &uid, const QString &yaml);
    bool isFileBusy() const;
    void removeProfile(const QString &uid);
    void renameProfile(const QString &uid, const QString &name);
    void setUpdateInterval(const QString &uid, int minutes);

    /// Synchronous compatibility API for non-UI callers and deterministic tests.
    QString generateRuntimeConfig();
    /// Generates an immutable launch config on a worker and emits runtimeConfigReady.
    void requestRuntimeConfig();
    void cancelRuntimeGeneration();
    bool isRuntimeBusy() const;

signals:
    void reloaded();
    void profilesChanged(const QVector<Profile> &profiles, const QString &currentUid);
    void currentProfileChanged(const QString &uid);
    void enhancementLog(const QStringList &logs);
    void profileUpdated(const QString &uid);
    void runtimeConfigReady(const QString &path);
    void runtimeBusyChanged(bool busy);
    void fileBusyChanged(bool busy);
    void profileCreated(const QString &uid);
    void profileContentSaved(const QString &uid, bool success);
    void errorOccurred(const QString &message);

private:
    enum class WriteKind { Import, Create, Save, Refresh };
    struct ProfileWrite { Profile profile; QByteArray contents; QString sourcePath; WriteKind kind; };
    void enqueueWrite(ProfileWrite request);
    void startNextWrite();
    void cancelFileOperations();
    struct RuntimeRequest;
    struct RuntimeResult;
    RuntimeRequest prepareRuntime();
    static RuntimeResult buildRuntime(const RuntimeRequest &request);
    void startPendingRuntime();
    void cancelDownloads();
    bool acceptsChanges();
    QString profilesDir() const;
    int indexOf(const QString &uid) const;
    QNetworkReply *fetch(const QString &url);
    bool save();
    void refreshDueProfiles();

    QNetworkAccessManager *network_;
    QTimer *autoUpdate_;
    QVector<Profile> profiles_;
    QString currentUid_;
    QString secret_;
    QPointer<ConfigEnhancer> enhancer_;
    QJsonObject runtimeOverrides_;
    QHash<QString, QNetworkReply *> updating_;
    QQueue<ProfileWrite> fileQueue_;
    bool fileRunning_ = false;
    quint64 fileGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> fileCancellation_;
    bool maintenance_ = false;
    bool shuttingDown_ = false;
    bool runtimeRunning_ = false;
    bool runtimeRequested_ = false;
    quint64 runtimeGeneration_ = 0;
    std::shared_ptr<std::atomic_bool> runtimeCancellation_;
};

}  // namespace core
