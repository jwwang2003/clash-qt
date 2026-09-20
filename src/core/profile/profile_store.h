#pragma once

#include <QDateTime>
#include <QObject>
#include <QVector>

#include "core/types.h"

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace core {

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

    /// Application data directory; profiles live in `<dataDir>/profiles`.
    QString dataDir() const;

    void load();
    QVector<Profile> profiles() const;
    QString currentUid() const;

    void selectProfile(const QString &uid);
    void importFromUrl(const QString &url, const QString &name = {});
    void importFromFile(const QString &path);
    void updateProfile(const QString &uid);
    void removeProfile(const QString &uid);
    void renameProfile(const QString &uid, const QString &name);
    void setUpdateInterval(const QString &uid, int minutes);

    /// Writes the selected profile out as the config the core should run,
    /// returning its absolute path. Empty when no profile is selected.
    QString generateRuntimeConfig();

signals:
    void profilesChanged(const QVector<Profile> &profiles, const QString &currentUid);
    void profileUpdated(const QString &uid);
    void runtimeConfigReady(const QString &path);
    void errorOccurred(const QString &message);

private:
    QString profilesDir() const;
    QString externalUiDir() const;
    void seedGeoDatabases() const;
    int indexOf(const QString &uid) const;
    QNetworkReply *fetch(const QString &url);
    void save();
    void refreshDueProfiles();

    QNetworkAccessManager *network_;
    QTimer *autoUpdate_;
    QVector<Profile> profiles_;
    QString currentUid_;
    QString secret_;
};

}  // namespace core
