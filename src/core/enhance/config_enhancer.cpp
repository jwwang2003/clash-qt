#include "core/enhance/config_enhancer.h"

#include "core/yaml_util.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlEngine>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace core {
namespace {

constexpr auto kIndexFile = "chain.json";
constexpr int kScriptTimeoutMs = 5000;
constexpr qint64 kMaxLogBytes = 1024 * 1024;
constexpr qsizetype kMaxSourceBytes = 16 * 1024 * 1024;

constexpr auto kMergeTemplate = R"(# Merge step: deep-merged into the profile's config.
# Nested maps merge key by key; scalars and sequences replace what was there.

profile:
  store-selected: true
)";

constexpr auto kScriptTemplate = R"(// Script step: receives the config as a plain
// object and returns the new one. console.log/info/warn/error/debug end up in
// the enhancement log.

function main(config, profileName) {
  return config;
}
)";

// Wired up as `console` before the step's own source runs.
constexpr auto kConsoleShim = R"(
function __text__(args) {
    var parts = [];
    for (var i = 0; i < args.length; i++) {
        var value = args[i];
        parts.push(typeof value === "string" ? value : JSON.stringify(value));
    }
    return parts.join(" ");
}
var console = Object.freeze({
    log: function () { __log__.write("log", __text__(arguments)); },
    info: function () { __log__.write("info", __text__(arguments)); },
    warn: function () { __log__.write("warn", __text__(arguments)); },
    error: function () { __log__.write("error", __text__(arguments)); },
    debug: function () { __log__.write("debug", __text__(arguments)); },
});
)";

QString appDataDir() {
    const QString custom = qEnvironmentVariable("CLASH_QT_DATA_DIR");
    const QString dir = custom.isEmpty()
        ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        : QFileInfo(custom).absoluteFilePath();
    QDir().mkpath(dir);
    return dir;
}

QString shortId() { return QUuid::createUuid().toString(QUuid::Id128).left(12); }

QString suffixFor(ChainKind kind) { return kind == ChainKind::Script ? ".js" : ".yaml"; }

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

bool readFile(const QString &path, QByteArray *data, QString *reason) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *reason = file.errorString();
        return false;
    }
    *data = file.read(kMaxSourceBytes + 1);
    if (data->size() > kMaxSourceBytes) {
        *reason = ConfigEnhancer::tr("Enhancement file exceeds the 16 MiB limit.");
        return false;
    }
    if (file.error() != QFileDevice::NoError) { *reason = file.errorString(); return false; }
    return true;
}

QJsonValue toJson(const YAML::Node &node, int depth = 0);

/// The type a plain (unquoted) scalar carries for a YAML reader.
QJsonValue scalarToJson(const YAML::Node &node) {
    const QString text = QString::fromStdString(node.Scalar());
    // A quoted scalar carries the "!" tag: it stays a string even if it looks
    // like a number, which is what keeps `password: "0123"` intact.
    return (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str")
        ? QJsonValue(text) : yamlutil::plainToJson(text);
}

QJsonValue toJson(const YAML::Node &node, int depth) {
    if (depth > 128) throw YAML::BadConversion(node.Mark());
    switch (node.Type()) {
        case YAML::NodeType::Map: {
            QJsonObject object;
            for (const auto &entry : node) {
                object.insert(QString::fromStdString(entry.first.Scalar()), toJson(entry.second, depth + 1));
            }
            return object;
        }
        case YAML::NodeType::Sequence: {
            QJsonArray array;
            for (const auto &item : node) array.append(toJson(item, depth + 1));
            return array;
        }
        case YAML::NodeType::Scalar:
            return scalarToJson(node);
        default:
            return QJsonValue::Null;
    }
}

YAML::Node toYaml(const QJsonValue &value) {
    switch (value.type()) {
        case QJsonValue::Object: {
            YAML::Node node(YAML::NodeType::Map);
            const QJsonObject object = value.toObject();
            for (auto it = object.begin(); it != object.end(); ++it) {
                node[it.key().toStdString()] = toYaml(it.value());
            }
            return node;
        }
        case QJsonValue::Array: {
            YAML::Node node(YAML::NodeType::Sequence);
            for (const QJsonValue &item : value.toArray()) node.push_back(toYaml(item));
            return node;
        }
        case QJsonValue::Bool:
            return YAML::Node(value.toBool());
        case QJsonValue::Double: {
            const double number = value.toDouble();
            if (!std::isfinite(number) ||
                number < static_cast<double>(std::numeric_limits<qint64>::min()) ||
                number >= static_cast<double>(std::numeric_limits<qint64>::max())) {
                return YAML::Node(number);
            }
            const auto integer = static_cast<qint64>(number);
            // JSON has one number type; ports and timeouts must not come back as 7890.0.
            return static_cast<double>(integer) == number ? YAML::Node(integer)
                                                          : YAML::Node(number);
        }
        case QJsonValue::String: {
            YAML::Node node(value.toString().toStdString());
            node.SetTag("!");  // marks it a string for the emitter, as the parser does
            return node;
        }
        default:
            return YAML::Node(YAML::NodeType::Null);
    }
}

YAML::Node lowercaseKeys(const YAML::Node &source) {
    YAML::Node result(YAML::NodeType::Map);
    for (const auto &entry : source) {
        if (!entry.first.IsScalar()) throw YAML::BadConversion(entry.first.Mark());
        const std::string key = QString::fromStdString(entry.first.Scalar()).toLower().toStdString();
        result[key] = YAML::Clone(entry.second);
    }
    return result;
}

/// Maps merge key by key; a scalar or a sequence replaces the value wholesale.
YAML::Node merged(const YAML::Node &base, const YAML::Node &fragment, int depth = 0) {
    if (depth > 128) throw YAML::BadConversion(fragment.Mark());
    if (!base.IsMap() || !fragment.IsMap()) return YAML::Clone(fragment);

    YAML::Node result = YAML::Clone(base);
    for (const auto &entry : fragment) {
        const std::string key = entry.first.Scalar();
        result[key] = result[key] ? merged(result[key], entry.second, depth + 1) : YAML::Clone(entry.second);
    }
    return result;
}

/// Sink for the `console` the shim installs; kept out of the JS heap so the
/// diagnostics survive a script that is interrupted or throws.
class ScriptConsole : public QObject {
    Q_OBJECT

public:
    Q_INVOKABLE void write(const QString &level, const QString &message) {
        if (bytes_ > kMaxLogBytes) return;

        bytes_ += level.size() + message.size();
        if (bytes_ > kMaxLogBytes) {
            logs.append(QString("error: log output truncated at %1 bytes").arg(kMaxLogBytes));
            return;
        }
        logs.append(level + ": " + message);
    }

    QStringList logs;

private:
    qint64 bytes_ = 0;
};

/// QJSEngine::setInterrupted has to come from a thread other than the one
/// running the script, so the deadline is driven from here.
class Watchdog {
public:
    Watchdog(QJSEngine *engine, int timeoutMs,
             const std::shared_ptr<std::atomic_bool> &cancelled = {}) {
        thread_ = std::thread([this, engine, timeoutMs, cancelled] {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
            std::unique_lock<std::mutex> lock(mutex_);
            while (!finished_) {
                if ((cancelled && cancelled->load()) || std::chrono::steady_clock::now() >= deadline) {
                    fired_ = true;
                    engine->setInterrupted(true);
                    return;
                }
                idle_.wait_for(lock, std::chrono::milliseconds(25), [this] { return finished_; });
            }
        });
    }

    ~Watchdog() { stop(); }

    /// Returns whether the script was interrupted.
    bool stop() {
        if (!thread_.joinable()) return fired_;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            finished_ = true;
        }
        idle_.notify_one();
        thread_.join();
        return fired_;
    }

private:
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable idle_;
    bool finished_ = false;
    bool fired_ = false;
};

struct ScriptOutcome {
    QJsonObject config;  // the input unchanged on failure
    QStringList logs;
    QString error;
};

ScriptOutcome runScript(const QString &source, const QJsonObject &config,
                        const QString &profileName,
                        const std::shared_ptr<std::atomic_bool> &cancelled = {}) {
    ScriptOutcome outcome;
    outcome.config = config;

    // Declared first so it outlives the engine holding its wrapper.
    ScriptConsole console;
    QJSEngine engine;
    QQmlEngine::setObjectOwnership(&console, QQmlEngine::CppOwnership);
    engine.globalObject().setProperty("__log__", engine.newQObject(&console));
    engine.evaluate(kConsoleShim);

    const QByteArray json = QJsonDocument(config).toJson(QJsonDocument::Compact);
    Watchdog watchdog(&engine, kScriptTimeoutMs, cancelled);

    const QJSValue jsonApi = engine.globalObject().property("JSON");
    const QJSValue parse = jsonApi.property("parse");
    const QJSValue stringify = jsonApi.property("stringify");
    QJSValue value = engine.evaluate(source);
    if (!value.isError()) {
        const QJSValue main = engine.globalObject().property("main");
        if (!main.isCallable()) {
            outcome.error = ConfigEnhancer::tr("no main() function");
        } else {
            value = main.call({parse.call({QString::fromUtf8(json)}), profileName});
            if (!value.isError()) value = stringify.call({value});
        }
    }

    if (watchdog.stop()) {
        // An interrupt surfaces as an ordinary error value; name the real cause.
        outcome.error = ConfigEnhancer::tr("timed out after %1 s").arg(kScriptTimeoutMs / 1000.0);
    } else if (value.isError()) {
        outcome.error = value.toString();
    }

    if (outcome.error.isEmpty()) {
        const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8());
        if (document.isObject()) {
            outcome.config = document.object();
        } else {
            outcome.error = ConfigEnhancer::tr("main() did not return a config object");
        }
    }

    outcome.logs = console.logs;
    return outcome;
}

QJsonObject toJson(const ChainItem &item) {
    return QJsonObject{
        {"uid", item.uid},
        {"name", item.name},
        {"kind", item.kind == ChainKind::Script ? "script" : "merge"},
        {"file", QFileInfo(item.filePath).fileName()},
        {"enabled", item.enabled},
    };
}

ChainItem fromJson(const QJsonObject &entry, const QString &dir) {
    ChainItem item;
    item.uid = entry.value("uid").toString();
    item.name = entry.value("name").toString();
    item.kind = entry.value("kind").toString() == "script" ? ChainKind::Script : ChainKind::Merge;
    item.filePath = dir + '/' + entry.value("file").toString();
    item.enabled = entry.value("enabled").toBool(true);
    return item;
}

}  // namespace

ConfigEnhancer::ConfigEnhancer(QObject *parent) : QObject(parent) {}

ConfigEnhancer::~ConfigEnhancer() { cancelFileOperations(); }

void ConfigEnhancer::setMaintenanceMode(bool enabled) {
    maintenance_ = enabled;
    if (enabled) cancelFileOperations();
}

void ConfigEnhancer::beginShutdown() { shuttingDown_ = true; }

bool ConfigEnhancer::acceptsChanges() {
    if (!maintenance_ && !shuttingDown_) return true;
    emit errorOccurred(tr("Enhancement changes are paused while a backup operation is in progress."));
    return false;
}

QString ConfigEnhancer::chainDir() const {
    const QString dir = appDataDir() + "/chain";
    QDir().mkpath(dir);
    return dir;
}

int ConfigEnhancer::indexOf(const QString &uid) const {
    for (int i = 0; i < chain_.size(); ++i) {
        if (chain_[i].uid == uid) return i;
    }
    return -1;
}

bool ConfigEnhancer::save() {
    QJsonArray entries;
    for (const ChainItem &item : chain_) entries.append(toJson(item));

    const QJsonDocument document{QJsonObject{{"chain", entries}}};

    QString reason;
    if (!writeFile(appDataDir() + '/' + kIndexFile, document.toJson(QJsonDocument::Indented),
                   &reason)) {
        emit errorOccurred(tr("Could not save the enhancement chain: %1").arg(reason));
        return false;
    }
    return true;
}

void ConfigEnhancer::load() {
    emit reloaded();
    cancelFileOperations();
    chain_.clear();

    QFile file(appDataDir() + '/' + kIndexFile);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) {
            emit errorOccurred(tr("The enhancement index is not a valid JSON object."));
            emit chainChanged(chain_);
            return;
        }
        const QJsonObject index = document.object();
        const QString dir = chainDir();
        for (const QJsonValue &entry : index.value("chain").toArray()) {
            const QJsonObject object = entry.toObject();
            const QString name = object.value("file").toString();
            const ChainItem item = fromJson(object, dir);
            if (item.uid.isEmpty() || indexOf(item.uid) >= 0 ||
                name.isEmpty() || name == "." || name == ".." ||
                name.contains('/') || name.contains('\\') ||
                QFileInfo(item.filePath).isSymLink()) {
                emit errorOccurred(tr("Skipped an invalid enhancement index entry."));
                continue;
            }
            chain_.append(item);
        }
    }

    emit chainChanged(chain_);
}

QVector<ChainItem> ConfigEnhancer::chain() const { return chain_; }

void ConfigEnhancer::addItem(const QString &name, ChainKind kind, const QByteArray &contents) {
    if (!acceptsChanges()) return;
    ChainItem item;
    item.uid = shortId();
    item.name = name.isEmpty() ? tr("Untitled") : name;
    item.kind = kind;
    item.filePath = chainDir() + '/' + item.uid + suffixFor(kind);

    QString reason;
    if (!writeFile(item.filePath, contents, &reason)) {
        emit errorOccurred(tr("Could not create %1: %2").arg(item.name, reason));
        return;
    }

    chain_.append(item);
    save();
    emit chainChanged(chain_);
}

void ConfigEnhancer::addMerge(const QString &name) {
    addItem(name, ChainKind::Merge, kMergeTemplate);
}

void ConfigEnhancer::addScript(const QString &name) {
    addItem(name, ChainKind::Script, kScriptTemplate);
}

void ConfigEnhancer::importItem(const QString &path, ChainKind kind) {
    if (!acceptsChanges()) return;
    QByteArray contents;
    QString reason;
    if (!readFile(path, &contents, &reason)) {
        emit errorOccurred(tr("Could not read %1: %2").arg(path, reason));
        return;
    }

    if (kind == ChainKind::Merge) {
        try {
            if (!YAML::Load(contents.toStdString()).IsMap()) {
                emit errorOccurred(tr("%1 is not a YAML mapping").arg(path));
                return;
            }
        } catch (const YAML::Exception &error) {
            emit errorOccurred(
                tr("%1 is not valid YAML: %2").arg(path, QString::fromStdString(error.what())));
            return;
        }
    }

    addItem(QFileInfo(path).completeBaseName(), kind, contents);
}

bool ConfigEnhancer::saveItemContent(const QString &uid, const QString &contents) {
    if (!acceptsChanges()) return false;
    const int index = indexOf(uid);
    if (index < 0) return false;
    const ChainItem item = chain_[index];
    if (item.kind == ChainKind::Merge) {
        try {
            if (!YAML::Load(contents.toStdString()).IsMap()) {
                emit errorOccurred(tr("The merge must be a YAML mapping."));
                return false;
            }
        } catch (const YAML::Exception &error) {
            emit errorOccurred(tr("The merge is not valid YAML: %1").arg(QString::fromStdString(error.what())));
            return false;
        }
    }
    QString reason;
    if (!writeFile(item.filePath, contents.toUtf8(), &reason)) {
        emit errorOccurred(tr("Could not save %1: %2").arg(item.name, reason));
        return false;
    }
    emit chainChanged(chain_);
    return true;
}

bool ConfigEnhancer::isFileBusy() const { return fileRunning_; }

void ConfigEnhancer::cancelFileOperations() {
    ++fileGeneration_;
    if (fileCancellation_) fileCancellation_->store(true);
    const auto queued = fileQueue_;
    fileQueue_.clear();
    for (const auto &request : queued)
        if (!request.create) emit itemContentSaved(request.item.uid, false);
}

void ConfigEnhancer::importItemAsync(const QString &path, ChainKind kind) {
    if (!acceptsChanges()) return;
    ChainItem item;
    item.uid = shortId();
    item.name = QFileInfo(path).completeBaseName();
    item.kind = kind;
    item.filePath = chainDir() + '/' + item.uid + suffixFor(kind);
    enqueueWrite({item, {}, path, true});
}

void ConfigEnhancer::saveItemContentAsync(const QString &uid, const QString &contents) {
    if (!acceptsChanges()) { emit itemContentSaved(uid, false); return; }
    const int index = indexOf(uid);
    if (index < 0) { emit itemContentSaved(uid, false); return; }
    enqueueWrite({chain_[index], contents.toUtf8(), {}, false});
}

void ConfigEnhancer::enqueueWrite(ItemWrite request) {
    if (maintenance_ || shuttingDown_) return;
    fileQueue_.enqueue(std::move(request));
    if (!fileRunning_) startNextWrite();
}

void ConfigEnhancer::startNextWrite() {
    if (fileQueue_.isEmpty() || maintenance_) return;
    const ItemWrite request = fileQueue_.dequeue();
    if (!request.create && indexOf(request.item.uid) < 0) {
        emit itemContentSaved(request.item.uid, false);
        startNextWrite();
        return;
    }
    const quint64 generation = fileGeneration_;
    const auto cancelled = std::make_shared<std::atomic_bool>(false);
    fileCancellation_ = cancelled;
    fileRunning_ = true;
    emit fileBusyChanged(true);
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, request, generation, cancelled] {
        const QString error = watcher->result();
        watcher->deleteLater();
        bool success = generation == fileGeneration_ && !maintenance_ && !cancelled->load() && error.isEmpty();
        if (success) {
            if (request.create) {
                chain_.append(request.item);
                success = save();
                if (!success) chain_.removeLast();
            }
            else success = indexOf(request.item.uid) >= 0;
            if (success) emit chainChanged(chain_);
        }
        if (!error.isEmpty() && !cancelled->load() && generation == fileGeneration_)
            emit errorOccurred(tr("Could not save %1: %2").arg(request.item.name, error));
        if (!success && request.create) QFile::remove(request.item.filePath);
        if (!request.create) emit itemContentSaved(request.item.uid, success);
        fileRunning_ = false;
        if (!fileQueue_.isEmpty() && !maintenance_) startNextWrite();
        else emit fileBusyChanged(false);
    });
    watcher->setFuture(QtConcurrent::run([request, cancelled] {
        QByteArray contents = request.contents;
        QString error;
        if (!request.sourcePath.isEmpty() && !readFile(request.sourcePath, &contents, &error)) return error;
        if (cancelled->load()) return QString();
        if (contents.size() > kMaxSourceBytes) return ConfigEnhancer::tr("Enhancement file exceeds the 16 MiB limit.");
        if (request.item.kind == ChainKind::Merge) {
            try {
                if (!YAML::Load(contents.toStdString()).IsMap())
                    return ConfigEnhancer::tr("The merge must be a YAML mapping.");
            } catch (const YAML::Exception &exception) { return QString::fromStdString(exception.what()); }
        }
        if (cancelled->load()) return QString();
        if (!writeFile(request.item.filePath, contents, &error, cancelled)) return error;
        return QString();
    }));
}

void ConfigEnhancer::removeItem(const QString &uid) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0) return;

    cancelFileOperations();
    QFile::remove(chain_[index].filePath);
    chain_.removeAt(index);
    save();
    emit chainChanged(chain_);
}

void ConfigEnhancer::setEnabled(const QString &uid, bool enabled) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0 || chain_[index].enabled == enabled) return;

    chain_[index].enabled = enabled;
    save();
    emit chainChanged(chain_);
}

void ConfigEnhancer::moveItem(const QString &uid, int toIndex) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0) return;

    const int target = std::clamp(toIndex, 0, static_cast<int>(chain_.size()) - 1);
    if (target == index) return;

    chain_.move(index, target);
    save();
    emit chainChanged(chain_);
}

void ConfigEnhancer::renameItem(const QString &uid, const QString &name) {
    if (!acceptsChanges()) return;
    const int index = indexOf(uid);
    if (index < 0 || name.isEmpty()) return;

    chain_[index].name = name;
    save();
    emit chainChanged(chain_);
}

EnhanceResult ConfigEnhancer::apply(const QString &baseYaml, const QString &profileName) const {
    return applyChain(baseYaml, profileName, chain_);
}

EnhanceResult ConfigEnhancer::applyChain(const QString &baseYaml, const QString &profileName,
                                        const QVector<ChainItem> &chain,
                                        const std::shared_ptr<std::atomic_bool> &cancelled) {
    EnhanceResult result;
    result.yaml = baseYaml;

    YAML::Node config;
    try {
        config = YAML::Load(baseYaml.toStdString());
    } catch (const YAML::Exception &error) {
        result.error = tr("The profile is not valid YAML: %1")
                           .arg(QString::fromStdString(error.what()));
        return result;
    }
    if (!config.IsMap()) {
        result.error = tr("The profile is not a YAML mapping");
        return result;
    }

    QStringList failures;
    auto fail = [&](const ChainItem &item, const QString &reason) {
        failures.append(tr("%1: %2").arg(item.name, reason));
        result.logs.append(QString("exception: %1: %2").arg(item.name, reason));
    };

    for (const ChainItem &item : chain) {
        if (cancelled && cancelled->load()) return result;
        if (!item.enabled) continue;

        QByteArray source;
        QString reason;
        if (!readFile(item.filePath, &source, &reason)) {
            fail(item, reason);
            continue;
        }

        if (item.kind == ChainKind::Merge) {
            try {
                const YAML::Node fragment = YAML::Load(source.toStdString());
                if (!fragment.IsMap()) {
                    fail(item, tr("not a YAML mapping"));
                    continue;
                }
                config = merged(config, lowercaseKeys(fragment));
            } catch (const YAML::Exception &error) {
                fail(item, QString::fromStdString(error.what()));
            }
            continue;
        }

        try {
            const ScriptOutcome outcome =
                runScript(QString::fromUtf8(source), toJson(lowercaseKeys(config)).toObject(), profileName, cancelled);
            result.logs += outcome.logs;
            if (!outcome.error.isEmpty()) {
                // A failing script leaves the chain running on the config it was handed.
                fail(item, outcome.error);
                continue;
            }
            config = toYaml(outcome.config);
        } catch (const YAML::Exception &error) {
            fail(item, QString::fromStdString(error.what()));
        }
    }

    const std::string rendered = yamlutil::dump(config);
    if (rendered.empty()) {
        failures.append(tr("could not render the enhanced config"));
    } else {
        result.yaml = QString::fromStdString(rendered);
    }

    result.error = failures.join('\n');
    return result;
}

}  // namespace core

#include "config_enhancer.moc"
