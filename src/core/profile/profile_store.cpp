#include "core/profile/profile_store.h"

#include "core/yaml_util.h"

#include <algorithm>

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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

#include "core/controller_discovery.h"

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

bool writeFile(const QString &path, const QByteArray &data, QString *reason) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
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
}

QString ProfileStore::dataDir() const {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    return dir;
}

QString ProfileStore::profilesDir() const {
    const QString dir = dataDir() + "/profiles";
    QDir().mkpath(dir);
    return dir;
}

/// mihomo downloads these itself when they are absent, but that runs before it
/// opens its controller, so a first launch on a slow link looks like a hang.
/// Seeding from a local Clash Verge Rev install skips the wait entirely.
void ProfileStore::seedGeoDatabases() const {
    static constexpr const char *kGeoFiles[] = {"Country.mmdb", "geoip.dat", "geosite.dat"};
    const QString seedDir = QFileInfo(vergeConfigPath()).absolutePath();

    for (const char *name : kGeoFiles) {
        const QString target = dataDir() + '/' + name;
        if (QFileInfo::exists(target)) continue;

        const QString seed = seedDir + '/' + name;
        if (QFileInfo(seed).isReadable()) QFile::copy(seed, target);
    }
}

QString ProfileStore::externalUiDir() const {
    // Left empty on purpose: mihomo fetches `external-ui-url` only into an
    // empty directory, and seeding whatever dashboard another client happens
    // to ship would serve a different app from this same origin.
    const QString dir = dataDir() + "/ui";
    QDir().mkpath(dir);
    return dir;
}

int ProfileStore::indexOf(const QString &uid) const {
    for (int i = 0; i < profiles_.size(); ++i) {
        if (profiles_[i].uid == uid) return i;
    }
    return -1;
}

void ProfileStore::save() {
    QJsonArray entries;
    for (const Profile &profile : profiles_) entries.append(toJson(profile));

    const QJsonDocument document{
        QJsonObject{{"profiles", entries}, {"currentUid", currentUid_}, {"secret", secret_}}};

    QString reason;
    if (!writeFile(dataDir() + '/' + kIndexFile, document.toJson(QJsonDocument::Indented),
                   &reason)) {
        emit errorOccurred(tr("Could not save the profile index: %1").arg(reason));
    }
}

void ProfileStore::load() {
    profiles_.clear();
    currentUid_.clear();
    secret_.clear();

    QFile file(dataDir() + '/' + kIndexFile);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject index = QJsonDocument::fromJson(file.readAll()).object();
        const QString dir = profilesDir();

        for (const QJsonValue &entry : index.value("profiles").toArray()) {
            profiles_.append(fromJson(entry.toObject(), dir));
        }
        currentUid_ = index.value("currentUid").toString();
        secret_ = index.value("secret").toString();
    }

    emit profilesChanged(profiles_, currentUid_);
}

QVector<Profile> ProfileStore::profiles() const { return profiles_; }

QString ProfileStore::currentUid() const { return currentUid_; }

void ProfileStore::selectProfile(const QString &uid) {
    if (uid == currentUid_ || indexOf(uid) < 0) return;
    currentUid_ = uid;
    save();
    emit profilesChanged(profiles_, currentUid_);
}

QNetworkReply *ProfileStore::fetch(const QString &url) {
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return network_->get(request);
}

void ProfileStore::importFromUrl(const QString &url, const QString &name) {
    QNetworkReply *reply = fetch(url);
    connect(reply, &QNetworkReply::finished, this, [this, reply, url, name] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(tr("Download failed: %1").arg(reply->errorString()));
            return;
        }

        const QByteArray body = reply->readAll();
        QString reason;
        if (!isClashConfig(body, &reason)) {
            emit errorOccurred(tr("%1 did not return a Clash configuration: %2").arg(url, reason));
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

        if (!writeFile(profile.filePath, body, &reason)) {
            emit errorOccurred(tr("Could not save %1: %2").arg(profile.name, reason));
            return;
        }

        profiles_.append(profile);
        if (currentUid_.isEmpty()) currentUid_ = profile.uid;
        save();
        emit profilesChanged(profiles_, currentUid_);
    });
}

void ProfileStore::importFromFile(const QString &path) {
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

    profiles_.append(profile);
    if (currentUid_.isEmpty()) currentUid_ = profile.uid;
    save();
    emit profilesChanged(profiles_, currentUid_);
}

void ProfileStore::updateProfile(const QString &uid) {
    const int index = indexOf(uid);
    if (index < 0) return;
    if (!profiles_[index].remote) {
        emit errorOccurred(tr("%1 is a local profile and cannot be updated").arg(
            profiles_[index].name));
        return;
    }

    QNetworkReply *reply = fetch(profiles_[index].url);
    connect(reply, &QNetworkReply::finished, this, [this, reply, uid] {
        reply->deleteLater();
        // the profile may have been removed while the download was in flight.
        const int index = indexOf(uid);
        if (index < 0) return;
        Profile &profile = profiles_[index];

        if (reply->error() != QNetworkReply::NoError) {
            emit errorOccurred(
                tr("Could not update %1: %2").arg(profile.name, reply->errorString()));
            return;
        }

        const QByteArray body = reply->readAll();
        QString reason;
        if (!isClashConfig(body, &reason)) {
            emit errorOccurred(
                tr("%1 did not return a Clash configuration: %2").arg(profile.url, reason));
            return;
        }
        if (!writeFile(profile.filePath, body, &reason)) {
            emit errorOccurred(tr("Could not save %1: %2").arg(profile.name, reason));
            return;
        }

        profile.updated = QDateTime::currentDateTime();
        profile.subscription = parseUserInfo(reply->rawHeader("subscription-userinfo"));
        save();
        emit profileUpdated(uid);
        emit profilesChanged(profiles_, currentUid_);
    });
}

void ProfileStore::removeProfile(const QString &uid) {
    const int index = indexOf(uid);
    if (index < 0) return;

    QFile::remove(profiles_[index].filePath);
    profiles_.removeAt(index);

    if (currentUid_ == uid) {
        currentUid_ = profiles_.isEmpty() ? QString() : profiles_.first().uid;
    }
    save();
    emit profilesChanged(profiles_, currentUid_);
}

void ProfileStore::renameProfile(const QString &uid, const QString &name) {
    const int index = indexOf(uid);
    if (index < 0 || name.isEmpty()) return;

    profiles_[index].name = name;
    save();
    emit profilesChanged(profiles_, currentUid_);
}

void ProfileStore::setUpdateInterval(const QString &uid, int minutes) {
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

QString ProfileStore::generateRuntimeConfig() {
    const int index = indexOf(currentUid_);
    if (index < 0) return {};
    const Profile &profile = profiles_[index];

    YAML::Node root;
    try {
        root = YAML::LoadFile(profile.filePath.toStdString());
    } catch (const YAML::Exception &error) {
        emit errorOccurred(
            tr("Could not read %1: %2").arg(profile.name, QString::fromStdString(error.what())));
        return {};
    }

    if (secret_.isEmpty()) {
        secret_ = QUuid::createUuid().toString(QUuid::Id128);
        save();
    }

    // The core is only reachable if we, not the subscription, own these.
    root["external-controller"] = kController;
    root["secret"] = secret_.toStdString();
    root["mixed-port"] = kMixedPort;
    // mixed-port serves both protocols; a subscription's own pair would add two
    // more inbound listeners beside it.
    root.remove("port");
    root.remove("socks-port");

    // Without this mihomo forgets every selector choice on restart and falls
    // back to each group's first member, which is rarely the one that works.
    root["profile"]["store-selected"] = true;

    seedGeoDatabases();
    root["external-ui"] = externalUiDir().toStdString();
    root["external-ui-url"] = kDashboardUrl;

    const std::string rendered = yamlutil::dump(root);
    if (rendered.empty()) {
        emit errorOccurred(tr("Could not render %1").arg(profile.name));
        return {};
    }

    const QString path = dataDir() + '/' + kRuntimeFile;
    QString reason;
    if (!writeFile(path, QByteArray::fromStdString(rendered), &reason)) {
        emit errorOccurred(tr("Could not write the runtime config: %1").arg(reason));
        return {};
    }

    emit runtimeConfigReady(path);
    return path;
}

}  // namespace core
