#include "core/profiles/profile_store.h"

#include "core/config/yaml_util.h"
#include "core/config/enhance/config_enhancer.h"

#include <algorithm>

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace core {
namespace {

// Many providers serve Clash yaml only to a Clash client and send an HTML
// landing page to anything else, so the request has to look like one.
constexpr auto kUserAgent = "clash-verge/v2.4.2";
constexpr auto kIndexFile = "profiles.json";
constexpr auto kRuntimeFile = "runtime.yaml";
constexpr auto kController = "127.0.0.1:29097";
constexpr auto kDashboardUrl =
    "https://github.com/Zephyruso/zashboard/releases/latest/download/dist.zip";
constexpr int kMixedPort = 27890;
constexpr int kAutoUpdateTickMs = 60000;
constexpr qsizetype kMaxProfileBytes = 16 * 1024 * 1024;

QString shortId() { return QUuid::createUuid().toString(QUuid::Id128).left(12); }

SubscriptionInfo parseUserInfo(const QByteArray &header) {
    SubscriptionInfo info;
    static const QRegularExpression field(R"((\w+)\s*=\s*(\d+))");

    auto matches = field.globalMatch(QString::fromUtf8(header));
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString key = match.captured(1).toLower();
        const quint64 value = match.captured(2).toULongLong();

        if (key == "upload") info.upload = value;
        else if (key == "download") info.download = value;
        else if (key == "total") info.total = value;
        else if (key == "expire" && value != 0) {
            info.expire = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(value));
        }
    }
    return info;
}

QString nameFromResponse(QNetworkReply *reply) {
    const QString disposition = QString::fromUtf8(reply->rawHeader("content-disposition"));

    static const QRegularExpression extended(R"(filename\*\s*=\s*[\w-]+''([^;]+))");
    if (const auto match = extended.match(disposition); match.hasMatch()) {
        return QUrl::fromPercentEncoding(match.captured(1).trimmed().toUtf8());
    }
    static const QRegularExpression plain(R"(filename\s*=\s*"?([^";]+))");
    if (const auto match = plain.match(disposition); match.hasMatch()) {
        return QUrl::fromPercentEncoding(match.captured(1).trimmed().toUtf8());
    }
    return reply->url().host();
}

bool isClashConfig(const QByteArray &body, QString *reason) {
    try {
        const YAML::Node root = YAML::Load(body.toStdString());
        if (!root.IsMap()) {
            *reason = ProfileStore::tr("not a YAML mapping");
            return false;
        }
        if (!root["proxies"] && !root["proxy-providers"]) {
            *reason = ProfileStore::tr("no proxies or proxy-providers section");
            return false;
        }
        return true;
    } catch (const YAML::Exception &error) {
        *reason = QString::fromStdString(error.what());
        return false;
    }
}

bool writeFile(const QString &path, const QByteArray &data, QString *reason,
               const std::shared_ptr<std::atomic_bool> &cancelled = {}) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        *reason = file.errorString();
        return false;
    }
    if (cancelled && cancelled->load()) { file.cancelWriting(); return false; }
    if (!file.commit()) {
        *reason = file.errorString();
        return false;
    }
    return true;
}

QJsonObject toJson(const Profile &profile) {
    return QJsonObject{
        {"uid", profile.uid},
        {"name", profile.name},
        {"remote", profile.remote},
        {"url", profile.url},
        {"file", QFileInfo(profile.filePath).fileName()},
        {"updated", profile.updated.isValid() ? profile.updated.toSecsSinceEpoch() : 0},
        {"updateIntervalMinutes", profile.updateIntervalMinutes},
        {"upload", QString::number(profile.subscription.upload)},
        {"download", QString::number(profile.subscription.download)},
        {"total", QString::number(profile.subscription.total)},
        {"expire", profile.subscription.expire.isValid()
                       ? profile.subscription.expire.toSecsSinceEpoch()
                       : 0},
    };
}

Profile fromJson(const QJsonObject &entry, const QString &dir) {
    Profile profile;
    profile.uid = entry.value("uid").toString();
    profile.name = entry.value("name").toString();
    profile.remote = entry.value("remote").toBool();
    profile.url = entry.value("url").toString();
    profile.filePath = dir + '/' + entry.value("file").toString();
    profile.updateIntervalMinutes = entry.value("updateIntervalMinutes").toInt();

    if (const qint64 updated = entry.value("updated").toInteger(); updated != 0) {
        profile.updated = QDateTime::fromSecsSinceEpoch(updated);
    }
    profile.subscription.upload = entry.value("upload").toString().toULongLong();
    profile.subscription.download = entry.value("download").toString().toULongLong();
    profile.subscription.total = entry.value("total").toString().toULongLong();
    if (const qint64 expire = entry.value("expire").toInteger(); expire != 0) {
        profile.subscription.expire = QDateTime::fromSecsSinceEpoch(expire);
    }
    return profile;
}

}  // namespace

ProfileStore::ProfileStore(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)), autoUpdate_(new QTimer(this)) {
    connect(autoUpdate_, &QTimer::timeout, this, &ProfileStore::refreshDueProfiles);
    autoUpdate_->start(kAutoUpdateTickMs);
    connect(this, &ProfileStore::runtimeBusyChanged, this, [this] { emit fileBusyChanged(isFileBusy()); });
}

ProfileStore::~ProfileStore() {
    cancelRuntimeGeneration();
    cancelFileOperations();
}

void ProfileStore::cancelDownloads() {
    for (QNetworkReply *reply : network_->findChildren<QNetworkReply *>()) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    updating_.clear();
}

void ProfileStore::setMaintenanceMode(bool enabled) {
    if (maintenance_ == enabled) return;
    maintenance_ = enabled;
    if (enabled) {
        autoUpdate_->stop();
        cancelDownloads();
        cancelRuntimeGeneration();
        cancelFileOperations();
    } else if (!shuttingDown_) {
        autoUpdate_->start(kAutoUpdateTickMs);
    }
}

void ProfileStore::beginShutdown() {
    shuttingDown_ = true;
    autoUpdate_->stop();
    cancelDownloads();
    cancelRuntimeGeneration();
}

bool ProfileStore::acceptsChanges() {
    if (!maintenance_ && !shuttingDown_) return true;
    emit errorOccurred(tr("Profile changes are paused while a backup operation is in progress."));
    return false;
}

QString ProfileStore::dataDir() const {
    const QString custom = qEnvironmentVariable("CLASH_QT_DATA_DIR");
    const QString dir = custom.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QFileInfo(custom).absoluteFilePath();
    QDir().mkpath(dir);
    return dir;
}

QString ProfileStore::profilesDir() const {
    const QString dir = dataDir() + "/profiles";
    QDir().mkpath(dir);
    return dir;
}

int ProfileStore::indexOf(const QString &uid) const {
    for (int i = 0; i < profiles_.size(); ++i) {
        if (profiles_[i].uid == uid) return i;
    }
    return -1;
}

bool ProfileStore::save() {
    QJsonArray entries;
    for (const Profile &profile : profiles_) entries.append(toJson(profile));

    const QJsonDocument document{
        QJsonObject{{"profiles", entries}, {"currentUid", currentUid_}, {"secret", secret_}}};

    QString reason;
    if (!writeFile(dataDir() + '/' + kIndexFile, document.toJson(QJsonDocument::Indented),
                   &reason)) {
        emit errorOccurred(tr("Could not save the profile index: %1").arg(reason));
        return false;
    }
    return true;
}

void ProfileStore::load() {
    emit reloaded();
    cancelFileOperations();
    cancelRuntimeGeneration();
    cancelDownloads();
    runtimeOverrides_ = {};
    QFile overridesFile(dataDir() + "/runtime-overrides.json");
    if (overridesFile.open(QIODevice::ReadOnly)) {
        runtimeOverrides_ = QJsonDocument::fromJson(overridesFile.readAll()).object();
    }
    profiles_.clear();
    currentUid_.clear();
    secret_.clear();

    QFile file(dataDir() + '/' + kIndexFile);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) {
            emit errorOccurred(tr("The profile index is not a valid JSON object."));
            emit profilesChanged(profiles_, currentUid_);
            return;
        }
        const QJsonObject index = document.object();
        const QString dir = profilesDir();

        for (const QJsonValue &entry : index.value("profiles").toArray()) {
            const QJsonObject object = entry.toObject();
            const QString name = object.value("file").toString();
            const Profile profile = fromJson(object, dir);
            if (profile.uid.isEmpty() || indexOf(profile.uid) >= 0 ||
                name.isEmpty() || name == "." || name == ".." ||
                name.contains('/') || name.contains('\\') ||
                QFileInfo(profile.filePath).isSymLink()) {
                emit errorOccurred(tr("Skipped an invalid profile index entry."));
                continue;
            }
            profiles_.append(profile);
        }
        currentUid_ = index.value("currentUid").toString();
        if (indexOf(currentUid_) < 0) {
            currentUid_ = profiles_.isEmpty() ? QString() : profiles_.first().uid;
        }
        secret_ = index.value("secret").toString();
    }

    emit profilesChanged(profiles_, currentUid_);
}

QVector<Profile> ProfileStore::profiles() const { return profiles_; }

QString ProfileStore::currentUid() const { return currentUid_; }

void ProfileStore::selectProfile(const QString &uid) {
    if (!acceptsChanges()) return;
    if (uid == currentUid_ || indexOf(uid) < 0) return;
    cancelRuntimeGeneration();
    currentUid_ = uid;
    save();
    emit profilesChanged(profiles_, currentUid_);
    emit currentProfileChanged(currentUid_);
}

QNetworkReply *ProfileStore::fetch(const QString &url) {
    const QUrl parsed(url);
    if (!parsed.isValid() || parsed.host().isEmpty() ||
        (parsed.scheme() != "http" && parsed.scheme() != "https")) {
        emit errorOccurred(tr("Subscription URL must be a valid HTTP or HTTPS URL."));
        return nullptr;
    }
    QNetworkRequest request{parsed};
    request.setTransferTimeout(30000);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network_->get(request);
    reply->setReadBufferSize(kMaxProfileBytes + 1);
    connect(reply, &QIODevice::readyRead, reply, [reply] {
        if (reply->bytesAvailable() > kMaxProfileBytes) reply->abort();
    });
    return reply;
}

void ProfileStore::importFromUrl(const QString &url, const QString &name) {
    if (!acceptsChanges()) return;
    QNetworkReply *reply = fetch(url);
    if (!reply) return;
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, name] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }

        Profile profile;
        profile.uid = shortId();
        profile.name = name.isEmpty() ? nameFromResponse(reply) : name;
        profile.remote = true;
        profile.url = url;
        profile.filePath = profilesDir() + '/' + profile.uid + ".yaml";
        profile.updated = QDateTime::currentDateTime();
        profile.subscription = parseUserInfo(reply->rawHeader("subscription-userinfo"));
        enqueueWrite({profile, reply->readAll(), {}, WriteKind::Import});
    });
}

void ProfileStore::importFromFile(const QString &path) {
    if (!acceptsChanges()) return;
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly)) {
        emit errorOccurred(tr("Could not read %1: %2").arg(path, source.errorString()));
        return;
    }

    const QByteArray body = source.readAll();
    QString reason;
    if (!isClashConfig(body, &reason)) {
        emit errorOccurred(tr("%1 is not a Clash configuration: %2").arg(path, reason));
        return;
    }

    Profile profile;
    profile.uid = shortId();
    profile.name = QFileInfo(path).completeBaseName();
    profile.filePath = profilesDir() + '/' + profile.uid + ".yaml";
    profile.updated = QDateTime::currentDateTime();

    if (!writeFile(profile.filePath, body, &reason)) {
        emit errorOccurred(tr("Could not save %1: %2").arg(profile.name, reason));
        return;
    }

    const bool selected = currentUid_.isEmpty();
    profiles_.append(profile);
    if (selected) currentUid_ = profile.uid;
    save();
    emit profilesChanged(profiles_, currentUid_);
    if (selected) emit currentProfileChanged(currentUid_);
}

bool ProfileStore::createLocalProfile(const QString &name, const QString &yaml) {
    if (!acceptsChanges()) return false;
    QString reason;
    const QByteArray body = yaml.toUtf8();
    if (!isClashConfig(body, &reason)) {
        emit errorOccurred(tr("The profile is not a Clash configuration: %1").arg(reason));
        return false;
    }
    Profile profile;
    profile.uid = shortId();
    profile.name = name.trimmed().isEmpty() ? tr("Untitled") : name.trimmed();
    profile.filePath = profilesDir() + '/' + profile.uid + ".yaml";
    profile.updated = QDateTime::currentDateTime();
    if (!writeFile(profile.filePath, body, &reason)) {
        emit errorOccurred(tr("Could not save %1: %2").arg(profile.name, reason));
        return false;
    }
    const bool selected = currentUid_.isEmpty();
    profiles_.append(profile);
    if (selected) currentUid_ = profile.uid;
    save();
    emit profilesChanged(profiles_, currentUid_);
    if (selected) emit currentProfileChanged(currentUid_);
    return true;
}

void ProfileStore::updateProfile(const QString &uid) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0) return;
    if (!profiles_[index].remote) {
        emit errorOccurred(tr("%1 is a local profile and cannot be updated").arg(
            profiles_[index].name));
        return;
    }

    if (updating_.contains(uid)) return;
    QNetworkReply *reply = fetch(profiles_[index].url);
    if (!reply) return;
    updating_.insert(uid, reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, uid] {
        reply->deleteLater();
        if (updating_.value(uid) != reply) return;
        updating_.remove(uid);
        // the profile may have been removed while the download was in flight.
        const int index = indexOf(uid);
        if (index < 0) return;
        Profile &profile = profiles_[index];

        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(
                tr("Could not update %1: %2").arg(profile.name, reply->errorString()));
            return;
        }

        Profile refreshed = profile;
        refreshed.updated = QDateTime::currentDateTime();
        refreshed.subscription = parseUserInfo(reply->rawHeader("subscription-userinfo"));
        enqueueWrite({refreshed, reply->readAll(), {}, WriteKind::Refresh});
    });
}

bool ProfileStore::setSubscriptionUrl(const QString &uid, const QString &url) {
    if (!acceptsChanges()) return false;
    const int index = indexOf(uid);
    if (index < 0) return false;
    if (!profiles_[index].remote) {
        emit errorOccurred(tr("%1 is a local profile and has no subscription URL.").arg(profiles_[index].name));
        return false;
    }
    const QString edited = url.trimmed();
    const QUrl parsed(edited);
    if (!parsed.isValid() || parsed.host().isEmpty() ||
        (parsed.scheme() != "http" && parsed.scheme() != "https")) {
        emit errorOccurred(tr("Subscription URL must be a valid HTTP or HTTPS URL."));
        return false;
    }
    const QString previous = profiles_[index].url;
    if (edited == previous) return true;
    cancelFileOperations();
    profiles_[index].url = edited;
    if (!save()) {
        profiles_[index].url = previous;
        return false;
    }
    // A response from the previous subscription must not replace the cache
    // after its source has changed. The reply identity also guards late signals.
    if (QNetworkReply *reply = updating_.take(uid)) {
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    emit profilesChanged(profiles_, currentUid_);
    return true;
}

bool ProfileStore::saveProfileContent(const QString &uid, const QString &yaml) {
    if (!acceptsChanges()) return false;
    cancelFileOperations();
    const int index = indexOf(uid);
    if (index < 0) return false;
    QString reason;
    const QByteArray contents = yaml.toUtf8();
    if (!isClashConfig(contents, &reason) ||
        !writeFile(profiles_[index].filePath, contents, &reason)) {
        emit errorOccurred(tr("Could not save %1: %2").arg(profiles_[index].name, reason));
        return false;
    }
    if (uid == currentUid_) cancelRuntimeGeneration();
    profiles_[index].updated = QDateTime::currentDateTime();
    save();
    emit profileUpdated(uid);
    emit profilesChanged(profiles_, currentUid_);
    return true;
}

bool ProfileStore::isFileBusy() const { return fileRunning_ || runtimeRunning_; }

void ProfileStore::cancelFileOperations() {
    ++fileGeneration_;
    if (fileCancellation_) fileCancellation_->store(true);
    const auto queued = fileQueue_;
    fileQueue_.clear();
    for (const auto &request : queued)
        if (request.kind == WriteKind::Save) emit profileContentSaved(request.profile.uid, false);
}

void ProfileStore::importFromFileAsync(const QString &path) {
    if (!acceptsChanges()) return;
    Profile profile;
    profile.uid = shortId();
    profile.name = QFileInfo(path).completeBaseName();
    profile.filePath = profilesDir() + '/' + profile.uid + ".yaml";
    enqueueWrite({profile, {}, path, WriteKind::Import});
}

void ProfileStore::createLocalProfileAsync(const QString &name, const QString &yaml) {
    if (!acceptsChanges()) return;
    Profile profile;
    profile.uid = shortId();
    profile.name = name.trimmed().isEmpty() ? tr("Untitled") : name.trimmed();
    profile.filePath = profilesDir() + '/' + profile.uid + ".yaml";
    enqueueWrite({profile, yaml.toUtf8(), {}, WriteKind::Create});
}

void ProfileStore::saveProfileContentAsync(const QString &uid, const QString &yaml) {
    if (!acceptsChanges()) { emit profileContentSaved(uid, false); return; }
    const int index = indexOf(uid);
    if (index < 0) { emit profileContentSaved(uid, false); return; }
    enqueueWrite({profiles_[index], yaml.toUtf8(), {}, WriteKind::Save});
}

void ProfileStore::enqueueWrite(ProfileWrite request) {
    if (maintenance_ || shuttingDown_) return;
    if (request.profile.uid == currentUid_) cancelRuntimeGeneration();
    fileQueue_.enqueue(std::move(request));
    if (!fileRunning_) startNextWrite();
}

void ProfileStore::startNextWrite() {
    if (fileQueue_.isEmpty() || maintenance_) return;
    const ProfileWrite request = fileQueue_.dequeue();
    const bool creating = request.kind == WriteKind::Import || request.kind == WriteKind::Create;
    if (!creating && indexOf(request.profile.uid) < 0) {
        if (request.kind == WriteKind::Save) emit profileContentSaved(request.profile.uid, false);
        startNextWrite();
        return;
    }
    const quint64 generation = fileGeneration_;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    fileCancellation_ = cancelled;
    fileRunning_ = true;
    emit fileBusyChanged(true);
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, request, generation, cancelled, creating] {
        const QString error = watcher->result();
        watcher->deleteLater();
        bool success = generation == fileGeneration_ && !maintenance_ && !cancelled->load() && error.isEmpty();
        if (success && creating) {
            Profile profile = request.profile;
            profile.updated = QDateTime::currentDateTime();
            const bool selected = currentUid_.isEmpty();
            profiles_.append(profile);
            if (selected) currentUid_ = profile.uid;
            success = save();
            if (!success) {
                profiles_.removeLast();
                if (selected) currentUid_.clear();
            }
            if (success) emit profilesChanged(profiles_, currentUid_);
            if (selected && success) emit currentProfileChanged(currentUid_);
            if (success && request.kind == WriteKind::Create && !shuttingDown_) emit profileCreated(profile.uid);
        } else if (success) {
            const int index = indexOf(request.profile.uid);
            success = index >= 0;
            if (success) {
                if (request.profile.uid == currentUid_) cancelRuntimeGeneration();
                profiles_[index].updated = QDateTime::currentDateTime();
                if (request.kind == WriteKind::Refresh) profiles_[index].subscription = request.profile.subscription;
                success = save();
                emit profileUpdated(request.profile.uid);
                emit profilesChanged(profiles_, currentUid_);
            }
        }
        if (!error.isEmpty() && !cancelled->load() && generation == fileGeneration_)
            emit errorOccurred(tr("Could not save %1: %2").arg(request.profile.name, error));
        if (!success && creating) QFile::remove(request.profile.filePath);
        if (request.kind == WriteKind::Save) emit profileContentSaved(request.profile.uid, success);
        fileRunning_ = false;
        if (!fileQueue_.isEmpty() && !maintenance_) startNextWrite();
        else emit fileBusyChanged(isFileBusy());
    });
    watcher->setFuture(QtConcurrent::run([request, cancelled] {
        QByteArray contents = request.contents;
        if (!request.sourcePath.isEmpty()) {
            QFile source(request.sourcePath);
            if (!source.open(QIODevice::ReadOnly)) return source.errorString();
            contents = source.read(kMaxProfileBytes + 1);
            if (source.error() != QFileDevice::NoError) return source.errorString();
        }
        if (cancelled->load()) return QString();
        if (contents.size() > kMaxProfileBytes) return ProfileStore::tr("Profile exceeds the 16 MiB limit.");
        QString error;
        if (!isClashConfig(contents, &error)) return error;
        if (cancelled->load()) return QString();
        if (!writeFile(request.profile.filePath, contents, &error, cancelled)) return error;
        return QString();
    }));
}

void ProfileStore::removeProfile(const QString &uid) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0) return;

    cancelFileOperations();
    if (uid == currentUid_) cancelRuntimeGeneration();
    QFile::remove(profiles_[index].filePath);
    profiles_.removeAt(index);

    const bool selected = currentUid_ == uid;
    if (selected) {
        currentUid_ = profiles_.isEmpty() ? QString() : profiles_.first().uid;
    }
    save();
    emit profilesChanged(profiles_, currentUid_);
    if (selected) emit currentProfileChanged(currentUid_);
}

void ProfileStore::renameProfile(const QString &uid, const QString &name) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0 || name.isEmpty()) return;

    if (uid == currentUid_) cancelRuntimeGeneration();
    profiles_[index].name = name;
    save();
    emit profilesChanged(profiles_, currentUid_);
}

void ProfileStore::setUpdateInterval(const QString &uid, int minutes) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0) return;

    profiles_[index].updateIntervalMinutes = std::max(0, minutes);
    save();
    emit profilesChanged(profiles_, currentUid_);
}

void ProfileStore::refreshDueProfiles() {
    const QDateTime now = QDateTime::currentDateTime();

    QStringList due;
    for (const Profile &profile : profiles_) {
        if (!profile.remote || profile.updateIntervalMinutes <= 0) continue;
        const qint64 interval = qint64(profile.updateIntervalMinutes) * 60;
        if (!profile.updated.isValid() || profile.updated.addSecs(interval) <= now) {
            due.append(profile.uid);
        }
    }
    for (const QString &uid : due) updateProfile(uid);
}

void ProfileStore::setSeedDir(const QString &dir) { seedDir_ = dir; }

QString ProfileStore::seedDir() const { return seedDir_; }

void ProfileStore::setEnhancer(ConfigEnhancer *enhancer) {
    if (enhancer_) enhancer_->disconnect(this);
    cancelRuntimeGeneration();
    enhancer_ = enhancer;
    if (enhancer_) connect(enhancer_, &ConfigEnhancer::chainChanged, this, &ProfileStore::cancelRuntimeGeneration);
}

QJsonObject ProfileStore::runtimeOverrides() const { return runtimeOverrides_; }

bool ProfileStore::setRuntimeOverrides(const QJsonObject &overrides) {
    if (!acceptsChanges()) return false;
    if (overrides.contains("mixed-port")) {
        const QJsonValue port = overrides.value("mixed-port");
        if (!port.isDouble() || port.toDouble() != port.toInt() ||
            port.toInt() < 1 || port.toInt() > 65535) {
            emit errorOccurred(tr("Mixed port must be an integer between 1 and 65535."));
            return false;
        }
    }
    QString reason;
    if (!writeFile(dataDir() + "/runtime-overrides.json", QJsonDocument(overrides).toJson(),
                   &reason)) {
        emit errorOccurred(tr("Could not save runtime settings: %1").arg(reason));
        return false;
    }
    cancelRuntimeGeneration();
    runtimeOverrides_ = overrides;
    return true;
}

struct ProfileStore::RuntimeRequest {
    Profile profile;
    QJsonObject overrides;
    QVector<ChainItem> chain;
    bool hasEnhancer = false;
    QString secret, dataDir, seedDir, configPath;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct ProfileStore::RuntimeResult {
    QString path, error, warning;
    QStringList logs;
};

ProfileStore::RuntimeRequest ProfileStore::prepareRuntime() {
    RuntimeRequest request;
    const int index = indexOf(currentUid_);
    if (index < 0) return request;
    if (secret_.isEmpty()) {
        secret_ = QUuid::createUuid().toString(QUuid::Id128);
        if (!save()) return request;
    }
    request.profile = profiles_[index];
    request.overrides = runtimeOverrides_;
    request.hasEnhancer = !enhancer_.isNull();
    if (enhancer_) request.chain = enhancer_->chain();
    request.secret = secret_;
    request.dataDir = dataDir();
    request.seedDir = seedDir_;
    request.configPath = request.dataDir + "/.runtime-" + QUuid::createUuid().toString(QUuid::Id128) + ".yaml";
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    return request;
}

void ProfileStore::cancelRuntimeGeneration() {
    ++runtimeGeneration_;
    runtimeRequested_ = false;
    if (runtimeCancellation_) runtimeCancellation_->store(true);
}

bool ProfileStore::isRuntimeBusy() const { return runtimeRunning_; }

void ProfileStore::requestRuntimeConfig() {
    if (!acceptsChanges()) return;
    cancelRuntimeGeneration();
    runtimeRequested_ = true;
    if (!runtimeRunning_) startPendingRuntime();
}

void ProfileStore::startPendingRuntime() {
    runtimeRequested_ = false;
    const RuntimeRequest request = prepareRuntime();
    if (request.profile.uid.isEmpty()) {
        runtimeRunning_ = false;
        emit runtimeBusyChanged(false);
        return;
    }
    const quint64 generation = runtimeGeneration_;
    runtimeCancellation_ = request.cancelled;
    runtimeRunning_ = true;
    emit runtimeBusyChanged(true);
    auto *watcher = new QFutureWatcher<RuntimeResult>(this);
    connect(watcher, &QFutureWatcher<RuntimeResult>::finished, this, [this, watcher, generation, request] {
        const RuntimeResult result = watcher->result();
        watcher->deleteLater();
        if (generation == runtimeGeneration_ && !maintenance_ && !request.cancelled->load()) {
            emit enhancementLog(result.logs);
            if (!result.warning.isEmpty()) emit errorOccurred(result.warning);
            if (!result.error.isEmpty()) emit errorOccurred(result.error);
            if (!result.path.isEmpty()) emit runtimeConfigReady(result.path);
        } else if (!result.path.isEmpty()) {
            QFile::remove(result.path);
        }
        runtimeRunning_ = false;
        if (runtimeRequested_ && !maintenance_) startPendingRuntime();
        else emit runtimeBusyChanged(false);
    });
    watcher->setFuture(QtConcurrent::run([request] { return buildRuntime(request); }));
}

QString ProfileStore::generateRuntimeConfig() {
    if (!acceptsChanges()) return {};
    cancelRuntimeGeneration();
    const RuntimeRequest request = prepareRuntime();
    if (request.profile.uid.isEmpty()) return {};
    const RuntimeResult result = buildRuntime(request);
    emit enhancementLog(result.logs);
    if (!result.warning.isEmpty()) emit errorOccurred(result.warning);
    if (!result.error.isEmpty()) emit errorOccurred(result.error);
    if (!result.path.isEmpty()) emit runtimeConfigReady(result.path);
    return result.path;
}

ProfileStore::RuntimeResult ProfileStore::buildRuntime(const RuntimeRequest &request) {
    RuntimeResult result;
    const Profile &profile = request.profile;
    if (request.cancelled->load()) return result;
    try {
        YAML::Node root = YAML::LoadFile(profile.filePath.toStdString());
        if (!root.IsMap()) {
            result.error = tr("%1 is not a YAML mapping").arg(profile.name);
            return result;
        }
        if (request.hasEnhancer) {
            const EnhanceResult enhanced = ConfigEnhancer::applyChain(
                QString::fromStdString(yamlutil::dump(root)), profile.name, request.chain, request.cancelled);
            if (request.cancelled->load()) return {};
            result.logs = enhanced.logs;
            result.warning = enhanced.error;
            root = YAML::Load(enhanced.yaml.toStdString());
            if (!root.IsMap()) {
                result.error = tr("The enhanced profile is not a YAML mapping");
                return result;
            }
        }
        const YAML::Node overrides = YAML::Load(
            QJsonDocument(request.overrides).toJson(QJsonDocument::Compact).toStdString());
        for (const auto &entry : overrides) {
            const std::string key = entry.first.as<std::string>();
            if (entry.second.IsMap() && root[key].IsMap()) {
                for (const auto &field : entry.second) {
                    root[key][field.first.as<std::string>()] = YAML::Clone(field.second);
                }
            } else {
                root[key] = YAML::Clone(entry.second);
            }
        }
        YAML::Node tun = root["tun"];
        if (!tun) {
            root["tun"] = YAML::Node(YAML::NodeType::Map);
            tun = root["tun"];
        } else if (!tun.IsMap()) {
            result.error = tr("The TUN configuration must be a YAML mapping.");
            return result;
        }
        if (!tun["enable"]) tun["enable"] = false;
        if (!tun["stack"]) tun["stack"] = "mixed";
        if (!tun["auto-route"]) tun["auto-route"] = true;
        if (!tun["auto-detect-interface"]) tun["auto-detect-interface"] = true;
        const YAML::Node effective = root;
        const YAML::Node dns = effective["dns"];
        // DNS interception requires an enabled internal resolver. Preserve
        // explicit interception settings, including an intentionally empty list.
        if (!tun["dns-hijack"] && dns && dns.IsMap() && dns["enable"] &&
            dns["enable"].as<bool>(false)) {
            tun["dns-hijack"] = YAML::Node(YAML::NodeType::Sequence);
            tun["dns-hijack"].push_back("any:53");
            tun["dns-hijack"].push_back("tcp://any:53");
        }
        root["external-controller"] = kController;
        root["secret"] = request.secret.toStdString();
        root["mixed-port"] = request.overrides.value("mixed-port").toInt(kMixedPort);
        root.remove("port");
        root.remove("socks-port");
        if (!root["profile"].IsMap()) root["profile"] = YAML::Node(YAML::NodeType::Map);
        root["profile"]["store-selected"] = true;
        if (request.cancelled->load()) return {};
        // Geo data is seeded, never overwritten: an engine that has already
        // refreshed Country.mmdb in dataDir keeps its copy. The directory comes
        // from the caller (ProfileStore::setSeedDir); empty means no seeding at
        // all, rather than probing the filesystem root for "/Country.mmdb".
        if (!request.seedDir.isEmpty()) {
            for (const char *name : {"Country.mmdb", "geoip.dat", "geosite.dat"}) {
                const QString target = request.dataDir + '/' + name;
                const QString source = request.seedDir + '/' + name;
                if (!QFileInfo::exists(target) && QFileInfo(source).isReadable())
                    QFile::copy(source, target);
            }
        }
        QDir().mkpath(request.dataDir + "/ui");
        root["external-ui"] = (request.dataDir + "/ui").toStdString();
        root["external-ui-url"] = kDashboardUrl;

        const std::string rendered = yamlutil::dump(root);
        if (rendered.empty()) {
            result.error = tr("Could not render %1").arg(profile.name);
            return result;
        }
        const QString path = request.configPath;
        QString reason;
        if (request.cancelled->load()) return {};
        if (!writeFile(path, QByteArray::fromStdString(rendered), &reason, request.cancelled)) {
            if (request.cancelled->load()) return {};
            result.error = tr("Could not write the runtime config: %1").arg(reason);
            return result;
        }
        // Keep a conventional preview path; launches use the immutable path.
        if (!writeFile(request.dataDir + '/' + kRuntimeFile, QByteArray::fromStdString(rendered), &reason, request.cancelled)) {
            QFile::remove(path);
            if (request.cancelled->load()) return {};
            result.error = tr("Could not write the runtime preview: %1").arg(reason);
            return result;
        }
        result.path = path;
        return result;
    } catch (const YAML::Exception &error) {
        result.error = tr("Could not generate %1: %2")
                           .arg(profile.name, QString::fromStdString(error.what()));
        return result;
    }
}

}  // namespace core
