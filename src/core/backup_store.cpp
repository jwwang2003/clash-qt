#include "core/backup_store.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QSemaphore>
#include <QThread>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QUuid>

#include <memory>
#include <atomic>

#include <yaml-cpp/yaml.h>

namespace core {

struct BackupOperation {
    QSemaphore permission;
    QSemaphore preparation;
    std::atomic<bool> cancelled{false};
    std::atomic<bool> approved{false};
    std::atomic<bool> success{false};
    bool preparationRequired = false;
};

class BackupWorker : public QObject {
    Q_OBJECT
public:
    BackupWorker(QString dataDir, std::function<bool(BackupStore &)> work,
                 std::shared_ptr<BackupOperation> operation)
        : dataDir_(std::move(dataDir)), work_(std::move(work)), operation_(std::move(operation)) {}

    void execute() {
        if (operation_->preparationRequired) operation_->preparation.acquire();
        BackupStore backend(dataDir_);
        connect(&backend, &BackupStore::errorOccurred, this, &BackupWorker::errorOccurred);
        connect(&backend, &BackupStore::statusChanged, this, &BackupWorker::statusChanged);
        connect(&backend, &BackupStore::backupsChanged, this, &BackupWorker::backupsChanged);
        connect(&backend, &BackupStore::restored, this, &BackupWorker::restored);
        backend.approveRestore_ = [this] {
            if (operation_->cancelled) return false;
            emit restorePrepared();
            operation_->permission.acquire();
            return operation_->approved && !operation_->cancelled;
        };
        if (!operation_->cancelled) operation_->success = work_(backend);
        emit finished();
    }

signals:
    void errorOccurred(const QString &message);
    void statusChanged(const QString &message);
    void backupsChanged();
    void restored();
    void restorePrepared();
    void finished();

private:
    QString dataDir_;
    std::function<bool(BackupStore &)> work_;
    std::shared_ptr<BackupOperation> operation_;
};

namespace {

constexpr qsizetype kMaxArchive = 32 * 1024 * 1024;
constexpr qsizetype kMaxFile = 16 * 1024 * 1024;
const QStringList roots{"profiles.json", "chain.json", "runtime-overrides.json", "profiles", "chain"};

bool safeName(const QString &name) {
    if (name.isEmpty() || name == "." || name == ".." || name.endsWith('.') || name.endsWith(' ')) return false;
    for (const auto character : name) {
        if (character.unicode() < 32 || QStringView(u"/\\:<>\"|?*").contains(character)) return false;
    }
    const QString base = name.section('.', 0, 0).toUpper();
    if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
        base == "CONIN$" || base == "CONOUT$") return false;
    if (base.size() == 4 && (base.startsWith("COM") || base.startsWith("LPT")) &&
        base.at(3) >= '1' && base.at(3) <= '9') return false;
    return true;
}

QString normalizedPath(const QString &path) {
    return path.normalized(QString::NormalizationForm_C).toCaseFolded();
}

bool safePath(const QString &path) {
    if (path == "profiles.json" || path == "chain.json" || path == "runtime-overrides.json") return true;
    const auto pieces = path.split('/');
    return pieces.size() == 2 && (pieces.first() == "profiles" || pieces.first() == "chain") &&
           safeName(pieces.last());
}

bool settingAllowed(const QString &key) {
    return key == "startup/startCore" || key == "core/binary" || key == "sysproxy/bypass" ||
           key == "window/geometry" || key.startsWith("hotkeys/") || key.startsWith("dashboard/");
}

bool writeFile(const QString &path, const QByteArray &data, QString *error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { *error = file.errorString(); return false; }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(data) != data.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

QByteArray readArchive(const QString &path, QString *error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { *error = file.errorString(); return {}; }
    if (file.size() > kMaxArchive) { *error = BackupStore::tr("Backup exceeds the 32 MiB limit."); return {}; }
    const auto data = file.read(kMaxArchive + 1);
    if (data.size() > kMaxArchive || file.error() != QFileDevice::NoError) {
        *error = BackupStore::tr("Could not read the backup within its size limit.");
        return {};
    }
    return data;
}

QJsonObject jsonObject(const QByteArray &bytes, bool *ok) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    *ok = error.error == QJsonParseError::NoError && document.isObject();
    return document.object();
}

}  // namespace

BackupStore::BackupStore(const QString &dataDir, QObject *parent)
    : QObject(parent), dataDir_(QDir(dataDir).absolutePath()), network_(new QNetworkAccessManager(this)) {}

BackupStore::~BackupStore() { cancelAsync(); }

void BackupStore::cancelAsync() {
    if (operation_) {
        operation_->cancelled = true;
        operation_->permission.release();
        operation_->preparation.release();
    }
    for (auto *reply : network_->findChildren<QNetworkReply *>()) reply->abort();
}

void BackupStore::continueRestore(bool approved) {
    if (!operation_) return;
    operation_->approved = approved;
    if (!approved) operation_->cancelled = true;
    operation_->permission.release();
}

void BackupStore::continuePreparation() {
    if (operation_) operation_->preparation.release();
}

void BackupStore::runAsync(std::function<bool(BackupStore &)> work,
                          std::function<void(bool)> completion, bool restoring) {
    if (isBusy()) return;
    localBusy_ = true;
    operation_ = std::make_shared<BackupOperation>();
    const auto operation = operation_;
    operation->preparationRequired = preparationRequired_;
    auto *thread = new QThread;
    auto *worker = new BackupWorker(dataDir_, std::move(work), operation);
    worker->moveToThread(thread);
    connect(thread, &QThread::started, worker, &BackupWorker::execute);
    connect(worker, &BackupWorker::finished, thread, &QThread::quit, Qt::DirectConnection);
    connect(thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(worker, &BackupWorker::errorOccurred, this, &BackupStore::errorOccurred);
    connect(worker, &BackupWorker::statusChanged, this, &BackupStore::statusChanged);
    connect(worker, &BackupWorker::backupsChanged, this, &BackupStore::backupsChanged);
    connect(worker, &BackupWorker::restored, this, &BackupStore::restored);
    connect(worker, &BackupWorker::restorePrepared, this, &BackupStore::restorePrepared);
    connect(thread, &QThread::finished, this,
            [this, operation, completion = std::move(completion), restoring] {
        if (operation_ != operation) return;
        operation_.reset();
        localBusy_ = false;
        emit localBusyChanged(false, restoring);
        if (completion && !operation->cancelled) completion(operation->success);
        emit busyChanged(isBusy());
        emit localOperationFinished(operation->success);
    });
    emit localBusyChanged(true, restoring);
    emit busyChanged(true);
    emit statusChanged(restoring ? tr("Preparing and validating restore…") : tr("Preparing backup…"));
    if (preparationRequired_) emit operationPreparing();
    thread->start();
}

void BackupStore::createLocalAsync() {
    runAsync([](BackupStore &store) { return store.createLocal(); });
}

void BackupStore::exportArchiveAsync(const QString &destination) {
    runAsync([destination](BackupStore &store) { return store.exportArchive(destination); });
}

void BackupStore::importArchiveAsync(const QString &source) {
    runAsync([source](BackupStore &store) { return store.importArchive(source); });
}

void BackupStore::restoreLocalAsync(const QString &source) {
    runAsync([source](BackupStore &store) { return store.createLocal() && store.restoreLocal(source); }, {}, true);
}

QStringList BackupStore::localBackups() const {
    const QDir dir(dataDir_ + "/backups");
    QStringList result;
    for (const auto &file : dir.entryInfoList({"*.cqtbackup"}, QDir::Files | QDir::NoSymLinks, QDir::Name | QDir::Reversed))
        result.append(file.absoluteFilePath());
    return result;
}

QByteArray BackupStore::snapshot(QString *error) const {
    QMap<QString, QByteArray> files;
    qsizetype totalBytes = 0;
    for (const auto &root : roots) {
        const QFileInfo info(dataDir_ + '/' + root);
        if (info.isSymLink()) { *error = tr("Cannot back up a symbolic link: %1").arg(root); return {}; }
        if (!info.exists()) continue;
        QStringList paths;
        if (info.isDir()) {
            const QDir dir(info.absoluteFilePath());
            for (const auto &entry : dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
                if (entry.isSymLink() || !entry.isFile()) {
                    *error = tr("Unsupported backup entry: %1").arg(entry.fileName());
                    return {};
                }
                paths.append(root + '/' + entry.fileName());
            }
        } else paths.append(root);
        for (const auto &path : paths) {
            QFile file(dataDir_ + '/' + path);
            if (!safePath(path) || !file.open(QIODevice::ReadOnly) || file.size() > kMaxFile) {
                *error = tr("Cannot read backup entry: %1").arg(path);
                return {};
            }
            const auto content = file.read(kMaxFile + 1);
            totalBytes += content.size();
            if (content.size() > kMaxFile || totalBytes > kMaxArchive * 3 / 4 ||
                file.error() != QFileDevice::NoError) {
                *error = tr("Backup contents exceed the size limit or could not be read.");
                return {};
            }
            files.insert(path, content);
        }
    }
    if (!files.contains("profiles.json")) files["profiles.json"] = R"({"profiles":[],"currentUid":""})";
    if (!files.contains("chain.json")) files["chain.json"] = R"({"chain":[]})";
    if (!files.contains("runtime-overrides.json")) files["runtime-overrides.json"] = "{}";
    bool valid;
    auto profiles = jsonObject(files.value("profiles.json"), &valid);
    if (!valid) { *error = tr("The profile index is invalid."); return {}; }
    profiles.remove("secret");
    files["profiles.json"] = QJsonDocument(profiles).toJson(QJsonDocument::Compact);

    QJsonArray entries;
    for (auto it = files.begin(); it != files.end(); ++it) {
        entries.append(QJsonObject{{"path", it.key()}, {"data", QString::fromLatin1(it.value().toBase64())},
                                   {"sha256", QString::fromLatin1(QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex())}});
    }
    QSettings preferences("clash-qt", "clash-qt");
    QJsonObject settings;
    for (const auto &key : preferences.allKeys()) {
        if (!settingAllowed(key)) continue;
        const auto value = preferences.value(key);
        const bool bytes = value.metaType().id() == QMetaType::QByteArray;
        settings.insert(key, QJsonObject{{"bytes", bytes}, {"value", bytes
            ? QJsonValue(QString::fromLatin1(value.toByteArray().toBase64())) : QJsonValue::fromVariant(value)}});
    }
    const auto archive = QJsonDocument(QJsonObject{{"format", "clash-qt-backup"}, {"version", 1},
        {"created", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}, {"files", entries},
        {"settings", settings}}).toJson(QJsonDocument::Compact);
    QMap<QString, QByteArray> validatedFiles;
    QMap<QString, QVariant> validatedSettings;
    if (!validate(archive, &validatedFiles, &validatedSettings, error)) return {};
    return archive;
}

bool BackupStore::validate(const QByteArray &archive, QMap<QString, QByteArray> *files,
                           QMap<QString, QVariant> *settings, QString *error) const {
    const auto fail = [&](const QString &message) { *error = message; return false; };
    if (archive.size() > kMaxArchive) return fail(tr("Backup exceeds the 32 MiB limit."));
    bool ok;
    const auto document = jsonObject(archive, &ok);
    if (!ok || document.value("format") != "clash-qt-backup" || document.value("version").toInt() != 1 ||
        !document.value("files").isArray() || !document.value("settings").isObject())
        return fail(tr("Not a supported clash-qt backup."));
    QSet<QString> paths;
    for (const auto &entry : document.value("files").toArray()) {
        if (!entry.isObject()) return fail(tr("Invalid backup entry."));
        const auto item = entry.toObject();
        const auto path = item.value("path").toString();
        if (!safePath(path) || paths.contains(normalizedPath(path)))
            return fail(tr("Unsafe or duplicate backup path: %1").arg(path));
        paths.insert(normalizedPath(path));
        const auto decoded = QByteArray::fromBase64Encoding(item.value("data").toString().toLatin1(),
                                                          QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.size() > kMaxFile) return fail(tr("Invalid or oversized backup content."));
        const auto hash = QCryptographicHash::hash(decoded.decoded, QCryptographicHash::Sha256).toHex();
        if (item.value("sha256").toString().toLatin1() != hash) return fail(tr("Backup checksum mismatch: %1").arg(path));
        files->insert(path, decoded.decoded);
    }
    for (const auto &root : roots.mid(0, 3)) {
        if (!files->contains(root)) return fail(tr("Backup is missing %1.").arg(root));
        jsonObject(files->value(root), &ok);
        if (!ok) return fail(tr("Invalid JSON in %1.").arg(root));
    }
    for (const auto &kind : {QString("profiles"), QString("chain")}) {
        const auto index = jsonObject(files->value(kind + ".json"), &ok);
        if (!index.value(kind).isArray()) return fail(tr("Invalid %1 index.").arg(kind));
        QSet<QString> ids;
        QSet<QString> referencedFiles;
        for (const auto &entry : index.value(kind).toArray()) {
            const auto item = entry.toObject();
            const auto id = item.value("uid").toString();
            const auto file = item.value("file").toString();
            if (id.isEmpty() || ids.contains(id) || !safeName(file) ||
                referencedFiles.contains(normalizedPath(file)) || !files->contains(kind + '/' + file))
                return fail(tr("Invalid or missing file in the %1 index.").arg(kind));
            ids.insert(id);
            referencedFiles.insert(normalizedPath(file));
            const bool script = kind == "chain" && item.value("kind").toString() == "script";
            if (!script) {
                try {
                    if (!YAML::Load(files->value(kind + '/' + file).toStdString()).IsMap())
                        return fail(tr("Configuration %1 is not a YAML mapping.").arg(file));
                } catch (const YAML::Exception &) { return fail(tr("Invalid YAML in %1.").arg(file)); }
            }
        }
        if (kind == "profiles") {
            const auto current = index.value("currentUid").toString();
            if (!current.isEmpty() && !ids.contains(current)) return fail(tr("The selected profile is missing."));
        }
    }
    const auto values = document.value("settings").toObject();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!settingAllowed(it.key()) || !it.value().isObject()) return fail(tr("Unsupported setting in backup."));
        const auto item = it.value().toObject();
        if (item.value("bytes").toBool()) {
            const auto value = QByteArray::fromBase64Encoding(item.value("value").toString().toLatin1(),
                                                             QByteArray::AbortOnBase64DecodingErrors);
            if (!value) return fail(tr("Invalid setting value."));
            settings->insert(it.key(), value.decoded);
        } else {
            const auto value = item.value("value");
            if (!value.isString() && !value.isBool() && !value.isDouble()) return fail(tr("Invalid setting value."));
            settings->insert(it.key(), value.toVariant());
        }
    }
    return true;
}

bool BackupStore::saveLocal(const QByteArray &archive) {
    const QString dir = dataDir_ + "/backups";
    QDir().mkpath(dir);
    const QString name = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz") + '-' +
                         QUuid::createUuid().toString(QUuid::Id128).left(6) + ".cqtbackup";
    QString error;
    if (!writeFile(dir + '/' + name, archive, &error)) { emit errorOccurred(error); return false; }
    emit backupsChanged();
    emit statusChanged(tr("Saved backup %1.").arg(name));
    return true;
}

bool BackupStore::createLocal() {
    QString error;
    const auto archive = snapshot(&error);
    if (archive.isEmpty()) { emit errorOccurred(error); return false; }
    return saveLocal(archive);
}

bool BackupStore::exportArchive(const QString &destination) {
    QString error;
    const auto archive = snapshot(&error);
    if (archive.isEmpty() || !writeFile(destination, archive, &error)) { emit errorOccurred(error); return false; }
    emit statusChanged(tr("Exported backup."));
    return true;
}

bool BackupStore::importArchive(const QString &source) {
    QString error;
    const auto archive = readArchive(source, &error);
    QMap<QString, QByteArray> files;
    QMap<QString, QVariant> settings;
    if (!error.isEmpty() || !validate(archive, &files, &settings, &error)) { emit errorOccurred(error); return false; }
    return saveLocal(archive);
}

bool BackupStore::restoreLocal(const QString &source) {
    QString error;
    const auto archive = readArchive(source, &error);
    QMap<QString, QByteArray> files;
    QMap<QString, QVariant> settings;
    if (!error.isEmpty() || !validate(archive, &files, &settings, &error)) { emit errorOccurred(error); return false; }
    QDir().mkpath(dataDir_);
    QTemporaryDir stage(dataDir_ + "/.restore-XXXXXX");
    QTemporaryDir previous(dataDir_ + "/.previous-XXXXXX");
    if (!stage.isValid() || !previous.isValid()) { emit errorOccurred(tr("Cannot stage the backup restore.")); return false; }
    for (const auto &root : roots) {
        if (QFileInfo(dataDir_ + '/' + root).isSymLink()) {
            emit errorOccurred(tr("Refusing to restore over a symbolic link: %1").arg(root));
            return false;
        }
    }
    for (const auto &dir : {QString("profiles"), QString("chain")}) QDir().mkpath(stage.path() + '/' + dir);
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (!writeFile(stage.path() + '/' + it.key(), it.value(), &error)) { emit errorOccurred(error); return false; }
    }
    QSettings preferences("clash-qt", "clash-qt");
    QMap<QString, QVariant> oldSettings;
    for (const auto &key : preferences.allKeys()) if (settingAllowed(key)) oldSettings.insert(key, preferences.value(key));
    QStringList moved, installed;
    const auto rollback = [&]() -> QString {
        bool ok = true;
        for (const auto &root : installed) {
            const QString path = dataDir_ + '/' + root;
            ok = (QFileInfo(path).isDir() ? QDir(path).removeRecursively() : QFile::remove(path)) && ok;
        }
        for (const auto &root : moved) ok = QDir().rename(previous.path() + '/' + root, dataDir_ + '/' + root) && ok;
        if (!ok) {
            previous.setAutoRemove(false);
            return tr(" Restore rollback needs attention; remaining original files are preserved in %1.").arg(previous.path());
        }
        return {};
    };
    if (approveRestore_ && !approveRestore_()) {
        emit errorOccurred(tr("Restore cancelled before replacing any files."));
        return false;
    }
    emit aboutToRestore();
    for (const auto &root : roots) {
        const QString target = dataDir_ + '/' + root;
        if (QFileInfo::exists(target)) {
            if (!QDir().rename(target, previous.path() + '/' + root)) {
                const auto recovery = rollback();
                emit errorOccurred(tr("Cannot preserve %1 for restore.").arg(root) + recovery);
                return false;
            }
            moved.append(root);
        }
        if (!QDir().rename(stage.path() + '/' + root, target)) {
            const auto recovery = rollback();
            emit errorOccurred(tr("Cannot restore %1.").arg(root) + recovery);
            return false;
        }
        installed.append(root);
    }
    for (const auto &key : oldSettings.keys()) preferences.remove(key);
    for (auto it = settings.begin(); it != settings.end(); ++it) preferences.setValue(it.key(), it.value());
    preferences.sync();
    if (preferences.status() != QSettings::NoError) {
        for (const auto &key : settings.keys()) preferences.remove(key);
        for (auto it = oldSettings.begin(); it != oldSettings.end(); ++it) preferences.setValue(it.key(), it.value());
        preferences.sync();
        const auto recovery = rollback();
        emit errorOccurred(tr("Could not restore application settings.") + recovery);
        return false;
    }
    emit restored();
    emit statusChanged(tr("Restored backup. The core is stopped; restart the app to apply all preferences."));
    return true;
}

void BackupStore::uploadWebDav(const QString &url, const QString &username, const QString &password) {
    const auto body = std::make_shared<QByteArray>();
    runAsync([body](BackupStore &store) {
        QString error;
        *body = store.snapshot(&error);
        if (body->isEmpty()) { emit store.errorOccurred(error); return false; }
        return true;
    }, [this, url, username, password, body](bool success) {
        if (success) transfer(url, username, password, true, *body);
    });
}

void BackupStore::downloadWebDav(const QString &url, const QString &username, const QString &password) {
    transfer(url, username, password, false);
}

void BackupStore::transfer(const QString &address, const QString &username, const QString &password,
                           bool upload, const QByteArray &body) {
    if (isBusy()) return;
    const QUrl url(address);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != "https" && url.scheme() != "http") ||
        !url.userInfo().isEmpty() || url.hasFragment()) {
        emit errorOccurred(tr("Enter an HTTP(S) WebDAV URL for the backup file, without embedded credentials."));
        return;
    }
    QNetworkRequest request(url);
    request.setTransferTimeout(60000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    if (!username.isEmpty() || !password.isEmpty())
        request.setRawHeader("Authorization", "Basic " + (username + ':' + password).toUtf8().toBase64());
    auto *reply = upload ? network_->put(request, body) : network_->get(request);
    reply->setReadBufferSize(kMaxArchive + 1);
    transferring_ = true;
    emit busyChanged(true);
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(120000);
    auto received = std::make_shared<QByteArray>();
    connect(reply, &QIODevice::readyRead, this, [reply, received] {
        received->append(reply->read(kMaxArchive + 1 - received->size()));
        if (received->size() > kMaxArchive) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, received, upload] {
        reply->deleteLater();
        transferring_ = false;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
            emit busyChanged(isBusy());
            emit errorOccurred(tr("WebDAV transfer failed (HTTP %1): %2").arg(status).arg(reply->errorString()));
            return;
        }
        if (upload) {
            emit busyChanged(isBusy());
            emit statusChanged(tr("Uploaded backup to WebDAV."));
            return;
        }
        received->append(reply->read(kMaxArchive + 1 - received->size()));
        runAsync([received](BackupStore &store) {
            QMap<QString, QByteArray> files;
            QMap<QString, QVariant> settings;
            QString error;
            if (!store.validate(*received, &files, &settings, &error)) {
                emit store.errorOccurred(error);
                return false;
            }
            return store.saveLocal(*received);
        });
    });
}

}  // namespace core

#include "backup_store.moc"
