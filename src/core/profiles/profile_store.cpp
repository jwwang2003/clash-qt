#include "core/profiles/profile_store.h"

#include "core/config/config_composer.h"
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
constexpr auto kPresetsFile = "presets.json";
// The recovery copy. Written from the same bytes, immediately after the primary
// save succeeds, so it is not "the previous document" but "this document, in a
// second place" -- which is what a corrupted or half-written presets.json needs.
constexpr auto kPresetsLastGoodFile = "presets.last-good.json";
// Loopback, always: the controller carries the secret that commands the core,
// so it is never offered beyond this machine. Only the port moves (see
// kControllerPortVariable in the header).
constexpr auto kControllerHost = "127.0.0.1";
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

/// The error-severity messages, one per line. Empty when there were none, which
/// is how callers tell "rejected" from "accepted with remarks".
QString errorText(const QVector<config::Diagnostic> &diagnostics) {
    QStringList messages;
    for (const config::Diagnostic &diagnostic : diagnostics)
        if (diagnostic.severity == QLatin1String("error")) messages.append(diagnostic.message);
    return messages.join('\n');
}

QString warningText(const QVector<config::Diagnostic> &diagnostics) {
    QStringList messages;
    for (const config::Diagnostic &diagnostic : diagnostics)
        if (diagnostic.severity == QLatin1String("warning")) messages.append(diagnostic.message);
    return messages.join('\n');
}

/// What one preset file on disk turned out to be.
///
/// `Absent` is deliberately its own answer. "There is no file" is a first run;
/// "the file is there and will not open" -- a permission change, a directory
/// left at the path, a failing disk -- is a document that EXISTS and cannot be
/// read, and answering both with an empty document is how every preset a user
/// has disappears without a word being said about it.
enum class PresetFileState { Absent, Unusable, Valid };

/// Reads and fully validates one preset file. `reason` is filled in for
/// `Unusable` and is the sentence a user is shown, so it names the cause rather
/// than the file.
PresetFileState readPresetFile(const QString &path, QJsonObject *document,
                               config::PresetDocument *parsed, QString *reason) {
    const QFileInfo info(path);
    // A dangling symlink does not "exist" and is certainly not nothing either.
    if (!info.exists() && !info.isSymLink()) return PresetFileState::Absent;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *reason = file.errorString();
        return PresetFileState::Unusable;
    }
    QJsonParseError parse{};
    const QJsonDocument read = QJsonDocument::fromJson(file.readAll(), &parse);
    if (!read.isObject()) {
        *reason = ProfileStore::tr("it is not a JSON object (%1)").arg(parse.errorString());
        return PresetFileState::Unusable;
    }
    QVector<config::Diagnostic> diagnostics;
    if (!config::parsePresetDocument(read.object(), parsed, &diagnostics)) {
        *reason = errorText(diagnostics);
        return PresetFileState::Unusable;
    }
    *document = read.object();
    return PresetFileState::Valid;
}

}  // namespace

ProfileStore::ProfileStore(QObject *parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)), autoUpdate_(new QTimer(this)) {
    config::registerMetaTypes();
    presetDocument_ = config::emptyPresetDocument();
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
    loadPresets();
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

// ------------------------------------------------------------------- presets

QJsonObject ProfileStore::presetDocument() const { return presetDocument_; }

QVector<config::Diagnostic> ProfileStore::lastPresetDiagnostics() const {
    return presetDiagnostics_;
}

// Loads the persisted presets, or explains what stopped it. Nothing here writes:
// a bad file is left exactly where it is, because overwriting it would destroy
// the only evidence of what went wrong and nobody has asked for a save.
//
// The one rule the rest of it serves: an empty document is adopted only when
// there is genuinely nothing to adopt. Every other outcome -- unreadable
// primary, missing primary with a recovery copy beside it, both files unusable
// while presets are already loaded -- either recovers or keeps what it has, and
// says so.
void ProfileStore::loadPresets() {
    const QString primaryPath = dataDir() + '/' + kPresetsFile;
    QJsonObject document;
    config::PresetDocument parsed;
    QString reason;
    const PresetFileState primary = readPresetFile(primaryPath, &document, &parsed, &reason);

    if (primary == PresetFileState::Valid) {
        presetDocument_ = document;
        presets_ = parsed;
        presetDiagnostics_.clear();
        return;
    }

    QVector<config::Diagnostic> diagnostics;
    if (primary == PresetFileState::Unusable) {
        diagnostics.append({QStringLiteral("error"), QStringLiteral("presets"), {},
                            tr("%1 could not be read: %2")
                                .arg(QLatin1String(kPresetsFile), reason)});
    }

    // The last-good copy stands in for a primary that is unusable AND for one
    // that has gone missing: it is written from the bytes of a save that
    // succeeded, so it is a whole document either way, and the newest one known
    // to load.
    QJsonObject recoveredDocument;
    config::PresetDocument recoveredPresets;
    QString recoveryReason;
    const PresetFileState lastGood =
        readPresetFile(dataDir() + '/' + kPresetsLastGoodFile, &recoveredDocument,
                       &recoveredPresets, &recoveryReason);

    // Nothing on disk at all, and nothing in memory to lose: a first run. The
    // empty document is the right answer and there is nothing to report.
    const bool inMemory = presetDocument_ != config::emptyPresetDocument();
    if (primary == PresetFileState::Absent && lastGood == PresetFileState::Absent && !inMemory) {
        presetDocument_ = config::emptyPresetDocument();
        presets_ = {};
        presetDiagnostics_.clear();
        return;
    }

    if (lastGood == PresetFileState::Valid) {
        presetDocument_ = recoveredDocument;
        presets_ = recoveredPresets;
        diagnostics.append(
            {QStringLiteral("warning"), QStringLiteral("presets"), {},
             primary == PresetFileState::Absent
                 ? tr("%1 is missing; the presets were recovered from %2. The next save "
                      "restores both copies.")
                       .arg(QLatin1String(kPresetsFile), QLatin1String(kPresetsLastGoodFile))
                 : tr("Recovered the last good preset document; %1 is unusable. The next save "
                      "replaces it.")
                       .arg(QLatin1String(kPresetsFile))});
        presetDiagnostics_ = diagnostics;
        emit errorOccurred(QStringList{errorText(diagnostics), warningText(diagnostics)}
                               .join('\n')
                               .trimmed());
        return;
    }
    if (lastGood == PresetFileState::Unusable) {
        diagnostics.append({QStringLiteral("error"), QStringLiteral("presets"), {},
                            tr("%1 could not be read either: %2")
                                .arg(QLatin1String(kPresetsLastGoodFile), recoveryReason)});
    }

    // Neither copy is usable. Presets already loaded are KEPT rather than
    // dropped: they are the user's, they are still valid, and this call was
    // asked to load a document -- not to throw one away because a disk answered
    // badly. A save made from here writes them back over both files.
    if (inMemory) {
        diagnostics.append(
            {QStringLiteral("warning"), QStringLiteral("presets"), {},
             tr("Kept the presets already loaded; no usable preset document is on disk.")});
    } else {
        presetDocument_ = config::emptyPresetDocument();
        presets_ = {};
        diagnostics.append({QStringLiteral("warning"), QStringLiteral("presets"), {},
                            tr("No usable preset document was found; no presets are applied.")});
    }
    presetDiagnostics_ = diagnostics;
    emit errorOccurred(QStringList{errorText(diagnostics), warningText(diagnostics)}
                           .join('\n')
                           .trimmed());
}

bool ProfileStore::setPresetDocument(const QJsonObject &document) {
    if (!acceptsChanges()) return false;

    QVector<config::Diagnostic> diagnostics;
    config::PresetDocument parsed;
    if (!config::parsePresetDocument(document, &parsed, &diagnostics)) {
        // Nothing has been touched: not presets_, not presetDocument_, not the
        // file, not the last-good copy. Rejecting a document must not be a way
        // to lose the one already stored.
        presetDiagnostics_ = diagnostics;
        emit errorOccurred(tr("The presets were not saved: %1").arg(errorText(diagnostics)));
        return false;
    }

    // Persist what was parsed, not what was handed in: the two differ only in
    // normalisation, and storing the parsed form is what makes presetDocument()
    // a fixed point.
    const QJsonObject normalised = config::presetDocumentToJson(parsed);
    const QByteArray bytes = QJsonDocument(normalised).toJson(QJsonDocument::Indented);
    QString reason;
    if (!writeFile(dataDir() + '/' + kPresetsFile, bytes, &reason)) {
        emit errorOccurred(tr("Could not save the presets: %1").arg(reason));
        return false;
    }
    // Best effort, and deliberately unchecked: the primary save has already
    // succeeded, so a failure here costs the next recovery, not this save.
    QString backupReason;
    writeFile(dataDir() + '/' + kPresetsLastGoodFile, bytes, &backupReason);

    presetDocument_ = normalised;
    presets_ = parsed;
    presetDiagnostics_ = diagnostics;
    cancelRuntimeGeneration();
    emit presetsChanged();
    return true;
}

struct ProfileStore::RuntimeRequest {
    Profile profile;
    QJsonObject overrides;
    ChainSnapshot chain;
    bool hasEnhancer = false;
    config::PresetDocument presets;
    config::ControllerFields controller;
    QString dataDir, seedDir, configPath;
    std::shared_ptr<std::atomic_bool> cancelled;
};

struct ProfileStore::PreviewRequest {
    Profile profile;
    bool hasProfile = false;
    bool blocked = false;
    QByteArray source;
    QString readError;
    ChainSnapshot chain;
    bool hasEnhancer = false;
    config::PresetDocument presets;
    QJsonObject overrides;
    config::ControllerFields controller;
    bool secretIsProvisional = false;
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
    // Snapshotted here, on the owning thread, so the worker enhances one
    // instant of the chain rather than whatever each file happens to contain
    // when its step is reached.
    if (enhancer_) request.chain = enhancer_->snapshot();
    request.presets = presets_;
    request.dataDir = dataDir();
    request.seedDir = seedDir_;
    request.controller = controllerFieldsFor(request.dataDir);
    request.configPath = request.dataDir + "/.runtime-" + QUuid::createUuid().toString(QUuid::Id128) + ".yaml";
    request.cancelled = std::make_shared<std::atomic_bool>(false);
    return request;
}

quint16 ProfileStore::controllerPort() {
    bool parsed = false;
    const uint port = qEnvironmentVariable(kControllerPortVariable).toUInt(&parsed);
    // Port 0 would tell the engine to pick one, which this application could
    // then never find, so it is refused along with everything else unusable and
    // the shipped default stands.
    if (!parsed || port == 0 || port > 65535) return kDefaultControllerPort;
    return static_cast<quint16>(port);
}

config::ControllerFields ProfileStore::controllerFieldsFor(const QString &dataDir) const {
    config::ControllerFields controller;
    controller.externalController =
        QString::fromLatin1(kControllerHost) + QLatin1Char(':') + QString::number(controllerPort());
    controller.secret = secret_;
    // The application default, and nothing more. Whether the user's stored
    // mixed-port override beats it is a question about values, and values are
    // composed in core/config/config_composer.cpp -- which is handed the same
    // overrides object. Resolving it here as well used to be the only reason
    // compose() honoured the override at all.
    controller.mixedPort = kMixedPort;
    controller.externalUi = dataDir + "/ui";
    controller.externalUiUrl = QLatin1String(kDashboardUrl);
    controller.storeSelected = true;
    return controller;
}

ProfileStore::PreviewRequest ProfileStore::preparePreview(const QString &profileUid) const {
    PreviewRequest request;
    if (maintenance_) {
        request.blocked = true;
        return request;
    }
    const int index = indexOf(profileUid.isEmpty() ? currentUid_ : profileUid);
    if (index < 0) return request;
    request.profile = profiles_[index];
    request.hasProfile = true;

    QFile source(request.profile.filePath);
    if (!source.open(QIODevice::ReadOnly)) {
        request.readError = source.errorString();
    } else {
        request.source = source.read(kMaxProfileBytes + 1);
        if (request.source.size() > kMaxProfileBytes)
            request.readError = tr("Profile exceeds the 16 MiB limit.");
        else if (source.error() != QFileDevice::NoError)
            request.readError = source.errorString();
    }

    request.hasEnhancer = !enhancer_.isNull();
    if (enhancer_) request.chain = enhancer_->snapshot();
    request.presets = presets_;
    request.overrides = runtimeOverrides_;
    request.controller = controllerFieldsFor(dataDir());
    // prepareRuntime() mints and SAVES a secret when there is none. A preview
    // must not: it would be a write, and the contract says a preview writes
    // nothing. The preview reports the absence instead of inventing a value.
    request.secretIsProvisional = secret_.isEmpty();
    return request;
}

void ProfileStore::requestEffectiveConfigPreview(const QString &profileUid) {
    if (shuttingDown_) return;
    // Supersede in-flight requests. The counter is the only thing that decides
    // which answer is delivered; a late worker still finishes, its result is
    // simply dropped.
    const quint64 generation = ++previewGeneration_;
    const PreviewRequest request = preparePreview(profileUid);
    auto *watcher = new QFutureWatcher<config::ComposeResult>(this);
    connect(watcher, &QFutureWatcher<config::ComposeResult>::finished, this,
            [this, watcher, generation] {
                const config::ComposeResult result = watcher->result();
                watcher->deleteLater();
                if (generation != previewGeneration_ || shuttingDown_) return;
                emit effectiveConfigPreviewReady(result);
            });
    watcher->setFuture(QtConcurrent::run([request] { return buildPreview(request); }));
}

config::ComposeResult ProfileStore::buildPreview(const PreviewRequest &request) {
    config::ComposeResult result;
    const auto reject = [&result](const QString &message) {
        result.ok = false;
        result.yaml.clear();
        result.diagnostics.append(
            {QStringLiteral("error"), QStringLiteral("source"), {}, message});
        return result;
    };
    if (request.blocked)
        return reject(tr("Previews are paused while a backup operation is in progress."));
    if (!request.hasProfile) return reject(tr("There is no profile to preview."));
    if (!request.readError.isEmpty())
        return reject(tr("Could not read %1: %2").arg(request.profile.name, request.readError));

    config::ComposeInput input;
    input.profileUid = request.profile.uid;
    input.profileName = request.profile.name;
    input.presets = request.presets;
    input.overrides = request.overrides;
    input.controller = request.controller;
    input.sourceYaml = QString::fromUtf8(request.source);

    if (request.hasEnhancer) {
        const EnhanceResult enhanced =
            ConfigEnhancer::applyChain(input.sourceYaml, request.profile.name, request.chain);
        input.logs = enhanced.logs;
        input.sourceYaml = enhanced.yaml;
        input.sourceLabel = QStringLiteral("legacy-chain");
        if (!enhanced.error.isEmpty())
            result.diagnostics.append({QStringLiteral("warning"), QStringLiteral("legacy-chain"),
                                       {}, enhanced.error});
    }

    const config::ComposeResult composed = config::compose(input);
    result.ok = composed.ok;
    result.yaml = composed.yaml;
    result.logs = composed.logs;
    result.provenance = composed.provenance;
    result.diagnostics += composed.diagnostics;
    // Only worth saying about a composition that succeeded: on a failure the
    // diagnostics are about the failure, and a note about the secret would
    // just be noise between the reader and the reason.
    if (request.secretIsProvisional && result.ok)
        result.diagnostics.append(
            {QStringLiteral("info"), QStringLiteral("controller"), QStringLiteral("/secret"),
             tr("The controller secret is assigned when the core is first launched.")});
    return result;
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

// Reads the profile, runs the legacy chain, hands the rest to the pure
// composer, and writes what comes back. Everything between "already-enhanced
// YAML" and "rendered document" -- presets, overrides, TUN defaults, the
// controller-owned fields -- now lives in core/config/config_composer.cpp and
// is reachable without a filesystem. What is left here is the I/O and only the
// I/O, which is the whole reason for the split.
ProfileStore::RuntimeResult ProfileStore::buildRuntime(const RuntimeRequest &request) {
    RuntimeResult result;
    const Profile &profile = request.profile;
    if (request.cancelled->load()) return result;

    config::ComposeInput input;
    input.profileUid = profile.uid;
    input.profileName = profile.name;
    input.presets = request.presets;
    input.overrides = request.overrides;
    input.controller = request.controller;

    try {
        const YAML::Node root = YAML::LoadFile(profile.filePath.toStdString());
        if (!root.IsMap()) {
            result.error = tr("%1 is not a YAML mapping").arg(profile.name);
            return result;
        }
        const std::string source = yamlutil::dump(root);
        if (source.empty()) {
            // A profile that cannot be serialised -- a cyclic alias, say -- is
            // reported as unrenderable, the same answer the single-pass version
            // reached at its final dump.
            result.error = tr("Could not render %1").arg(profile.name);
            return result;
        }
        input.sourceYaml = QString::fromStdString(source);
    } catch (const YAML::Exception &error) {
        result.error = tr("Could not generate %1: %2")
                           .arg(profile.name, QString::fromStdString(error.what()));
        return result;
    }

    if (request.hasEnhancer) {
        const EnhanceResult enhanced = ConfigEnhancer::applyChain(
            input.sourceYaml, profile.name, request.chain, request.cancelled);
        if (request.cancelled->load()) return {};
        result.logs = enhanced.logs;
        result.warning = enhanced.error;
        input.logs = enhanced.logs;
        input.sourceYaml = enhanced.yaml;
        input.sourceLabel = QStringLiteral("legacy-chain");
    }

    const config::ComposeResult composed = config::compose(input);
    if (const QString warning = warningText(composed.diagnostics); !warning.isEmpty())
        result.warning = QStringList{result.warning, warning}.join('\n').trimmed();
    if (!composed.ok) {
        // No candidate exists. Returning with an empty path is what keeps the
        // previous runtime file, and the running core, exactly as they were.
        result.error = errorText(composed.diagnostics);
        if (result.error.isEmpty()) result.error = tr("Could not compose %1").arg(profile.name);
        return result;
    }
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
    // The composer has already written this path into external-ui; the
    // directory itself is this side's job.
    QDir().mkpath(request.controller.externalUi);

    const QByteArray rendered = composed.yaml.toUtf8();
    const QString path = request.configPath;
    QString reason;
    if (request.cancelled->load()) return {};
    if (!writeFile(path, rendered, &reason, request.cancelled)) {
        if (request.cancelled->load()) return {};
        result.error = tr("Could not write the runtime config: %1").arg(reason);
        return result;
    }
    // Keep a conventional preview path; launches use the immutable path.
    if (!writeFile(request.dataDir + '/' + kRuntimeFile, rendered, &reason, request.cancelled)) {
        QFile::remove(path);
        if (request.cancelled->load()) return {};
        result.error = tr("Could not write the runtime preview: %1").arg(reason);
        return result;
    }
    result.path = path;
    return result;
}

}  // namespace core
