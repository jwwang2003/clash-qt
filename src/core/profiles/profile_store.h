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

#include "core/config/config_composer.h"
#include "core/config/enhance/config_enhancer.h"
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
    ~ProfileStore() override;

    /// Application data directory; profiles live in `<dataDir>/profiles`.
    QString dataDir() const;

    /// Directory the engine's geo-data seed files (Country.mmdb, geoip.dat,
    /// geosite.dat) are copied from into dataDir() the first time a runtime
    /// config is generated, so a fresh install does not re-download them.
    ///
    /// SUPPLIED BY THE CALLER, and deliberately so. This store used to locate
    /// the directory itself by calling core::vergeConfigPath(), which made
    /// clash_profiles -- and everything that links it, up to and including the
    /// application -- depend on the component-private engine library for one
    /// path string (PRE-ARCH edge 1). The composition root already knows where
    /// the engine keeps its configuration; it says so here.
    ///
    /// Empty is the default and means "do not seed": no file is read, and no
    /// file is written beside the generated config. A store that is never told
    /// where to seed from therefore touches nothing outside dataDir(), which is
    /// what a test wants. Set it before the first generation; prepareRuntime()
    /// samples it per request, so a later change affects later generations only.
    void setSeedDir(const QString &dir);
    QString seedDir() const;

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

    // -------------------------------------------------------------- presets
    //
    // Contract revision config-r1, .refactor/P4_CONFIG_CONTRACT.md. The document
    // shape is {"version":1,"global":[Preset],"profiles":{"uid":[Preset]}}; the
    // composer owns its meaning (core/config/config_composer.h) and this owns
    // its persistence.

    /// The document as persisted: normalised, so two saves of equivalent input
    /// produce equal objects. Never invalid -- an unreadable document on disk is
    /// recovered or replaced before it is ever returned from here.
    ///
    /// load() adopts the empty document only when there is nothing anywhere to
    /// adopt. A primary file that is missing, unopenable or unusable is recovered
    /// from the last-good copy; when neither copy can be read, presets already
    /// loaded are KEPT. Every outcome but a genuine first run leaves a reason on
    /// lastPresetDiagnostics() and reports through errorOccurred().
    QJsonObject presetDocument() const;

    /// Validates, then persists atomically. Returns false and changes NOTHING --
    /// not the in-memory document, not the file, not the last-good copy -- when
    /// the document is rejected; the reasons are on lastPresetDiagnostics() and
    /// summarised through errorOccurred(). Emits presetsChanged() on success.
    bool setPresetDocument(const QJsonObject &document);

    /// Composes the configuration the selected profile (or `profileUid`, when
    /// given) WOULD produce, on a worker, and answers with
    /// effectiveConfigPreviewReady.
    ///
    /// Deliberately inert: it writes no file, seeds nothing, cancels no runtime
    /// generation and starts no backend. The input is snapshotted here, on the
    /// owning thread, so the answer belongs to one instant. Results from
    /// superseded requests are dropped rather than delivered out of order.
    void requestEffectiveConfigPreview(const QString &profileUid = {});

    /// Why the last load or setPresetDocument() said what it said. Empty when
    /// the last one was clean.
    QVector<config::Diagnostic> lastPresetDiagnostics() const;

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
    void presetsChanged();
    void effectiveConfigPreviewReady(const core::config::ComposeResult &result);

private:
    enum class WriteKind { Import, Create, Save, Refresh };
    struct ProfileWrite { Profile profile; QByteArray contents; QString sourcePath; WriteKind kind; };
    void enqueueWrite(ProfileWrite request);
    void startNextWrite();
    void cancelFileOperations();
    struct RuntimeRequest;
    struct RuntimeResult;
    struct PreviewRequest;
    RuntimeRequest prepareRuntime();
    PreviewRequest preparePreview(const QString &profileUid) const;
    config::ControllerFields controllerFieldsFor(const QString &dataDir) const;
    static RuntimeResult buildRuntime(const RuntimeRequest &request);
    static config::ComposeResult buildPreview(const PreviewRequest &request);
    void loadPresets();
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
    QString seedDir_;
    QPointer<ConfigEnhancer> enhancer_;
    QJsonObject runtimeOverrides_;
    QJsonObject presetDocument_;
    config::PresetDocument presets_;
    QVector<config::Diagnostic> presetDiagnostics_;
    quint64 previewGeneration_ = 0;
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
