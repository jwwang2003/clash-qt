#include "core/config/config_composer.h"

#include "core/config/yaml_util.h"

#include <yaml-cpp/yaml.h>

#include <algorithm>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace core::config {
namespace {

// A merge fragment comes from JSON, so its depth is already bounded by Qt's
// parser. The guard is here anyway: recursion driven by untrusted input with no
// floor is how a preset editor turns into a stack overflow.
constexpr int kMaxMergeDepth = 64;

QString text(const char *source) {
    return QCoreApplication::translate("core::config", source);
}

QString escapeToken(const QString &token) {
    QString escaped = token;
    escaped.replace(QLatin1Char('~'), QLatin1String("~0"));
    escaped.replace(QLatin1Char('/'), QLatin1String("~1"));
    return escaped;
}

QString encodePointer(const QStringList &tokens) {
    QString pointer;
    for (const QString &token : tokens) pointer += QLatin1Char('/') + escapeToken(token);
    return pointer;
}

void diagnose(QVector<Diagnostic> *diagnostics, const char *severity, const QString &source,
              const QString &path, const QString &message) {
    if (diagnostics) diagnostics->append({QString::fromLatin1(severity), source, path, message});
}

bool hasError(const QVector<Diagnostic> &diagnostics, int from) {
    for (int i = from; i < diagnostics.size(); ++i)
        if (diagnostics[i].severity == QLatin1String("error")) return true;
    return false;
}

// JSON is a subset of YAML, so the parser is the converter. Going through the
// text form is not a shortcut taken for brevity: it is what gives every JSON
// string an explicit YAML string tag, so a preset that sets a password to
// "0123" or "0xFF" survives the round trip as a string instead of coming back
// to mihomo as a number. The runtime-override path has always done this; presets
// now get the same guarantee from the same mechanism.
YAML::Node jsonToYaml(const QJsonValue &value) {
    const QJsonDocument document{QJsonObject{{QStringLiteral("v"), value}}};
    const YAML::Node wrapper = YAML::Load(document.toJson(QJsonDocument::Compact).toStdString());
    return wrapper[std::string("v")];
}

void deepMerge(YAML::Node target, const YAML::Node &fragment, int depth = 0) {
    if (depth >= kMaxMergeDepth) return;
    for (const auto &entry : fragment) {
        const std::string key = entry.first.as<std::string>();
        if (entry.second.IsMap() && target[key].IsDefined() && target[key].IsMap()) {
            deepMerge(target[key], entry.second, depth + 1);
        } else {
            target[key] = YAML::Clone(entry.second);
        }
    }
}

// How many levels deepMerge would actually recurse: only a level where both
// sides are maps recurses, which is exactly where the guard above could stop
// early. Answered before anything is written, and abandoned as soon as it
// reaches `limit`, because the only question is "does this fit?".
int alignedMergeDepth(const YAML::Node &target, const YAML::Node &fragment, int limit) {
    if (limit <= 0) return 0;
    int deepest = 0;
    for (const auto &entry : fragment) {
        if (!entry.second.IsMap()) continue;
        const YAML::Node into = target[entry.first.as<std::string>()];
        if (!into.IsDefined() || !into.IsMap()) continue;
        deepest = std::max(deepest, 1 + alignedMergeDepth(into, entry.second, limit - 1));
        if (deepest >= limit) break;
    }
    return deepest;
}

// True when the whole fragment lands. A merge that would run into the guard is
// refused rather than performed: half of a fragment is not a weaker version of
// the user's intent, it is a different configuration nobody asked for.
bool mergeFitsTheGuard(const YAML::Node &target, const YAML::Node &fragment) {
    return alignedMergeDepth(target, fragment, kMaxMergeDepth) < kMaxMergeDepth;
}

// Walks to the map that owns the pointer's last token. `create` fills in missing
// intermediate maps, which is what lets a preset set /dns/enable on a profile
// that has no dns block at all. An intermediate that exists but is not a map is
// never rewritten: that is the "bad parent" the contract wants diagnosed.
YAML::Node resolveParent(YAML::Node node, const QStringList &tokens, int index, bool create,
                         int *badToken) {
    if (index + 1 >= tokens.size()) return node;
    const std::string key = tokens[index].toStdString();
    if (!node[key].IsDefined()) {
        if (!create) {
            *badToken = index;
            return YAML::Node(YAML::NodeType::Undefined);
        }
        node[key] = YAML::Node(YAML::NodeType::Map);
    } else if (!node[key].IsMap()) {
        *badToken = index;
        return YAML::Node(YAML::NodeType::Undefined);
    }
    return resolveParent(node[key], tokens, index + 1, create, badToken);
}

const char *kOperationNames[] = {"merge", "replace", "remove", "prepend", "append"};

bool operationFromName(const QString &name, OperationKind *kind) {
    for (int i = 0; i < 5; ++i) {
        if (name == QLatin1String(kOperationNames[i])) {
            *kind = static_cast<OperationKind>(i);
            return true;
        }
    }
    return false;
}

// The pointers the application owns outright. /port and /socks-port are in the
// list because composition deletes them unconditionally: a preset that sets one
// is not merely overruled, it is inert, and saying so beats the silence.
const QVector<QStringList> &controllerOwnedPaths() {
    static const QVector<QStringList> paths{
        {QStringLiteral("external-controller")},
        {QStringLiteral("secret")},
        {QStringLiteral("mixed-port")},
        {QStringLiteral("external-ui")},
        {QStringLiteral("external-ui-url")},
        {QStringLiteral("port")},
        {QStringLiteral("socks-port")},
        {QStringLiteral("profile"), QStringLiteral("store-selected")},
    };
    return paths;
}

// ------------------------------------------------------------ the layers

// `fatal` is set when a layer could only be applied in part. It is separate
// from an error diagnostic on purpose: a refused operation (a protected path, a
// non-map parent) leaves a document that still means what the rest of the
// presets said, while a truncated one does not, so only the second kind may
// take the whole candidate down with it.
bool applyOperation(YAML::Node root, const Operation &operation, const QString &source,
                    ComposeResult *result, bool *fatal) {
    const QString path = operation.path;
    if (isControllerOwnedPath(operation.tokens)) {
        diagnose(&result->diagnostics, "error", source, path,
                 text("%1 is owned by the application and cannot be set by a preset.")
                     .arg(path));
        return false;
    }

    if (operation.kind == OperationKind::Prepend || operation.kind == OperationKind::Append) {
        const bool prepend = operation.kind == OperationKind::Prepend;
        if (root[std::string("rules")].IsDefined() && !root[std::string("rules")].IsSequence()) {
            diagnose(&result->diagnostics, "error", source, path,
                     text("/rules is not a sequence, so rules cannot be added to it."));
            return false;
        }
        YAML::Node rebuilt(YAML::NodeType::Sequence);
        const QJsonArray added = operation.value.toArray();
        if (prepend)
            for (const QJsonValue &rule : added) rebuilt.push_back(rule.toString().toStdString());
        if (root[std::string("rules")].IsDefined())
            for (const auto &rule : root[std::string("rules")]) rebuilt.push_back(YAML::Clone(rule));
        if (!prepend)
            for (const QJsonValue &rule : added) rebuilt.push_back(rule.toString().toStdString());
        root[std::string("rules")] = rebuilt;
        result->provenance.append({path, source});
        return true;
    }

    const bool create = operation.kind != OperationKind::Remove;
    int badToken = -1;
    YAML::Node parent = resolveParent(root, operation.tokens, 0, create, &badToken);
    if (badToken >= 0) {
        if (operation.kind == OperationKind::Remove) return false;  // nothing to remove
        diagnose(&result->diagnostics, "error", source,
                 encodePointer(operation.tokens.mid(0, badToken + 1)),
                 text("%1 is not a mapping, so %2 cannot be written.")
                     .arg(encodePointer(operation.tokens.mid(0, badToken + 1)), path));
        return false;
    }
    const std::string key = operation.tokens.last().toStdString();

    switch (operation.kind) {
    case OperationKind::Remove:
        // Idempotent by contract: removing what is not there is not a problem.
        if (!parent[key].IsDefined()) return false;
        parent.remove(key);
        break;
    case OperationKind::Replace:
        parent[key] = YAML::Clone(jsonToYaml(operation.value));
        break;
    case OperationKind::Merge: {
        const YAML::Node fragment = jsonToYaml(operation.value);
        if (fragment.IsMap() && parent[key].IsDefined() && parent[key].IsMap()) {
            if (!mergeFitsTheGuard(parent[key], fragment)) {
                diagnose(&result->diagnostics, "error", source, path,
                         text("The value merged into %1 nests maps more than %2 levels deep, "
                              "which is past the supported limit; flatten it or split it into "
                              "several operations. Nothing was composed.")
                             .arg(path)
                             .arg(kMaxMergeDepth));
                *fatal = true;
                return false;
            }
            deepMerge(parent[key], fragment);
        } else {
            parent[key] = YAML::Clone(fragment);
        }
        break;
    }
    case OperationKind::Prepend:
    case OperationKind::Append:
        break;  // handled above
    }
    result->provenance.append({path, source});
    return true;
}

void applyPresets(YAML::Node root, const QVector<Preset> &presets, const QString &layer,
                  ComposeResult *result, bool *fatal) {
    for (const Preset &preset : presets) {
        if (!preset.enabled) continue;
        const QString source = layer + QLatin1Char(':') + preset.id;
        for (const Operation &operation : preset.operations) {
            applyOperation(root, operation, source, result, fatal);
            // Past a fatal operation the document is already not the one any
            // layer described, so further diagnostics would be about a
            // configuration that is never going to exist.
            if (*fatal) return;
        }
    }
}

// The shallow, per-top-level-key merge the runtime overrides have always had:
// a map value merges key by key into an existing map, anything else replaces.
// Reproduced exactly -- widening it to a deep merge would silently change what
// every stored override does.
void applyOverrides(YAML::Node root, const QJsonObject &overrides, ComposeResult *result) {
    if (overrides.isEmpty()) return;
    const YAML::Node parsed =
        YAML::Load(QJsonDocument(overrides).toJson(QJsonDocument::Compact).toStdString());
    for (const auto &entry : parsed) {
        const std::string key = entry.first.as<std::string>();
        const QString name = QString::fromStdString(key);
        // mixed-port is settled by the controller layer, which is the only place
        // that knows what the default is; writing it here too would put two
        // provenance entries on one key and answer neither honestly.
        if (name == QLatin1String("mixed-port")) continue;
        if (isControllerOwnedPath({name})) {
            // Not rejected: it is inert anyway, because the controller-owned
            // fields are reasserted below. Said out loud so a user who set one
            // is not left wondering why nothing happened.
            diagnose(&result->diagnostics, "warning", QStringLiteral("override"),
                     QLatin1Char('/') + escapeToken(name),
                     text("/%1 is owned by the application; the override is ignored.").arg(name));
        }
        if (entry.second.IsMap() && root[key].IsMap()) {
            for (const auto &field : entry.second) {
                const std::string subkey = field.first.as<std::string>();
                root[key][subkey] = YAML::Clone(field.second);
                result->provenance.append(
                    {encodePointer({name, QString::fromStdString(subkey)}),
                     QStringLiteral("override")});
            }
        } else {
            root[key] = YAML::Clone(entry.second);
            result->provenance.append({QLatin1Char('/') + escapeToken(name),
                                       QStringLiteral("override")});
        }
    }
}

bool applyDefaults(YAML::Node root, ComposeResult *result) {
    const QString source = QStringLiteral("default");
    if (!root[std::string("tun")].IsDefined()) {
        root[std::string("tun")] = YAML::Node(YAML::NodeType::Map);
    } else if (!root[std::string("tun")].IsMap()) {
        diagnose(&result->diagnostics, "error", source, QStringLiteral("/tun"),
                 text("The TUN configuration must be a YAML mapping."));
        return false;
    }
    YAML::Node tun = root[std::string("tun")];
    const struct {
        const char *key;
        bool value;
    } flags[] = {{"enable", false}, {"auto-route", true}, {"auto-detect-interface", true}};
    for (const auto &flag : flags) {
        if (tun[std::string(flag.key)].IsDefined()) continue;
        tun[std::string(flag.key)] = flag.value;
        result->provenance.append({encodePointer({QStringLiteral("tun"),
                                                  QString::fromLatin1(flag.key)}), source});
    }
    if (!tun[std::string("stack")].IsDefined()) {
        tun[std::string("stack")] = "mixed";
        result->provenance.append({QStringLiteral("/tun/stack"), source});
    }

    // DNS interception requires an enabled internal resolver. Preserve explicit
    // interception settings, including an intentionally empty list.
    const YAML::Node dns = root[std::string("dns")];
    if (!tun[std::string("dns-hijack")].IsDefined() && dns && dns.IsMap() &&
        dns[std::string("enable")] && dns[std::string("enable")].as<bool>(false)) {
        YAML::Node hijack(YAML::NodeType::Sequence);
        hijack.push_back("any:53");
        hijack.push_back("tcp://any:53");
        tun[std::string("dns-hijack")] = hijack;
        result->provenance.append({QStringLiteral("/tun/dns-hijack"), source});
    }
    return true;
}

// The local proxy port is the one application-owned field a trusted runtime
// override may choose: the user picks where their browser points, a preset never
// does. HEAD resolved it inside ProfileStore
// (`runtimeOverrides_.value("mixed-port").toInt(kMixedPort)`), which left
// compose() answering "27890" to a caller who had just asked for 41234 --
// published-API behaviour nobody could have wanted. Resolving it here is what
// makes the pure function's answer the real answer.
//
// Validity is the store's own rule (ProfileStore::setRuntimeOverrides): an
// integer in 1..65535. Anything else cannot be a port, so the default stands and
// the user is told, rather than the engine being handed a number it will refuse.
struct MixedPort {
    int value = 0;
    QString source;
};

MixedPort resolveMixedPort(const ComposeInput &input, ComposeResult *result) {
    const QString key = QStringLiteral("mixed-port");
    const MixedPort owned{input.controller.mixedPort, QStringLiteral("controller")};
    if (!input.overrides.contains(key)) return owned;

    const QJsonValue chosen = input.overrides.value(key);
    const bool integral = chosen.isDouble() && chosen.toDouble() == chosen.toInt();
    if (!integral || chosen.toInt() < 1 || chosen.toInt() > 65535) {
        diagnose(&result->diagnostics, "warning", QStringLiteral("override"),
                 QStringLiteral("/mixed-port"),
                 text("/mixed-port must be an integer between 1 and 65535; the override is "
                      "ignored and %1 is used.")
                     .arg(input.controller.mixedPort));
        return owned;
    }
    return {chosen.toInt(), QStringLiteral("override")};
}

void applyControllerFields(YAML::Node root, const ControllerFields &controller,
                           const MixedPort &mixedPort, ComposeResult *result) {
    const QString source = QStringLiteral("controller");
    root[std::string("external-controller")] = controller.externalController.toStdString();
    root[std::string("secret")] = controller.secret.toStdString();
    root[std::string("mixed-port")] = mixedPort.value;
    root.remove(std::string("port"));
    root.remove(std::string("socks-port"));
    if (!root[std::string("profile")].IsMap())
        root[std::string("profile")] = YAML::Node(YAML::NodeType::Map);
    root[std::string("profile")][std::string("store-selected")] = controller.storeSelected;
    root[std::string("external-ui")] = controller.externalUi.toStdString();
    root[std::string("external-ui-url")] = controller.externalUiUrl.toStdString();

    for (const char *path : {"/external-controller", "/secret", "/profile/store-selected",
                             "/external-ui", "/external-ui-url"})
        result->provenance.append({QString::fromLatin1(path), source});
    // Written by this layer either way, but chosen by whoever supplied the value.
    result->provenance.append({QStringLiteral("/mixed-port"), mixedPort.source});
}

// ----------------------------------------------------------- document parsing

bool parseOperation(const QJsonValue &entry, const QString &presetId, Operation *out,
                    QVector<Diagnostic> *diagnostics) {
    const QString source = QStringLiteral("preset:") + presetId;
    if (!entry.isObject()) {
        diagnose(diagnostics, "error", source, {}, text("An operation is not a JSON object."));
        return false;
    }
    const QJsonObject object = entry.toObject();
    const QJsonValue name = object.value(QStringLiteral("op"));
    if (!name.isString() || !operationFromName(name.toString(), &out->kind)) {
        diagnose(diagnostics, "error", source, {},
                 text("\"%1\" is not one of merge, replace, remove, prepend or append.")
                     .arg(name.toVariant().toString()));
        return false;
    }
    const QJsonValue path = object.value(QStringLiteral("path"));
    if (!path.isString()) {
        diagnose(diagnostics, "error", source, {}, text("An operation has no \"path\" string."));
        return false;
    }
    out->path = path.toString();
    QString error;
    if (!decodePointer(out->path, &out->tokens, &error)) {
        diagnose(diagnostics, "error", source, out->path,
                 text("\"%1\" is not a valid JSON pointer: %2").arg(out->path, error));
        return false;
    }
    if (isControllerOwnedPath(out->tokens)) {
        diagnose(diagnostics, "error", source, out->path,
                 text("%1 is owned by the application and cannot be set by a preset.")
                     .arg(out->path));
        return false;
    }

    out->value = object.value(QStringLiteral("value"));
    const bool sequenceOp =
        out->kind == OperationKind::Prepend || out->kind == OperationKind::Append;
    if (sequenceOp) {
        if (out->tokens != QStringList{QStringLiteral("rules")}) {
            diagnose(diagnostics, "error", source, out->path,
                     text("prepend and append are allowed at /rules only."));
            return false;
        }
        if (!out->value.isArray()) {
            diagnose(diagnostics, "error", source, out->path,
                     text("prepend and append need an array of rule strings."));
            return false;
        }
        for (const QJsonValue &rule : out->value.toArray()) {
            if (rule.isString()) continue;
            diagnose(diagnostics, "error", source, out->path,
                     text("prepend and append need an array of rule strings."));
            return false;
        }
    } else if (out->kind != OperationKind::Remove && out->value.isUndefined()) {
        diagnose(diagnostics, "error", source, out->path,
                 text("%1 needs a \"value\".").arg(name.toString()));
        return false;
    }
    return true;
}

bool parsePreset(const QJsonValue &entry, Preset *out, QSet<QString> *seenIds,
                 QVector<Diagnostic> *diagnostics) {
    const QString source = QStringLiteral("presets");
    if (!entry.isObject()) {
        diagnose(diagnostics, "error", source, {}, text("A preset is not a JSON object."));
        return false;
    }
    const QJsonObject object = entry.toObject();
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!id.isString() || id.toString().isEmpty()) {
        diagnose(diagnostics, "error", source, {}, text("A preset has no \"id\" string."));
        return false;
    }
    out->id = id.toString();
    if (seenIds->contains(out->id)) {
        diagnose(diagnostics, "error", source, {},
                 text("Preset id \"%1\" is used more than once.").arg(out->id));
        return false;
    }
    seenIds->insert(out->id);

    const QJsonValue name = object.value(QStringLiteral("name"));
    if (!name.isUndefined() && !name.isString()) {
        diagnose(diagnostics, "error", QStringLiteral("preset:") + out->id, {},
                 text("\"name\" must be a string."));
        return false;
    }
    out->name = name.isString() ? name.toString() : out->id;

    const QJsonValue enabled = object.value(QStringLiteral("enabled"));
    if (!enabled.isUndefined() && !enabled.isBool()) {
        diagnose(diagnostics, "error", QStringLiteral("preset:") + out->id, {},
                 text("\"enabled\" must be true or false."));
        return false;
    }
    out->enabled = enabled.isBool() ? enabled.toBool() : true;

    const QJsonValue operations = object.value(QStringLiteral("operations"));
    if (operations.isUndefined()) return true;
    if (!operations.isArray()) {
        diagnose(diagnostics, "error", QStringLiteral("preset:") + out->id, {},
                 text("\"operations\" must be an array."));
        return false;
    }
    bool ok = true;
    for (const QJsonValue &entry : operations.toArray()) {
        Operation operation;
        if (!parseOperation(entry, out->id, &operation, diagnostics)) {
            ok = false;
            continue;
        }
        out->operations.append(operation);
    }
    return ok;
}

bool parsePresetArray(const QJsonValue &value, const QString &where, QVector<Preset> *out,
                      QSet<QString> *seenIds, QVector<Diagnostic> *diagnostics) {
    if (value.isUndefined()) return true;
    if (!value.isArray()) {
        diagnose(diagnostics, "error", QStringLiteral("presets"), {},
                 text("\"%1\" must be an array of presets.").arg(where));
        return false;
    }
    bool ok = true;
    for (const QJsonValue &entry : value.toArray()) {
        Preset preset;
        if (!parsePreset(entry, &preset, seenIds, diagnostics)) {
            ok = false;
            continue;
        }
        out->append(preset);
    }
    return ok;
}

}  // namespace

// ---------------------------------------------------------------- published

bool decodePointer(const QString &pointer, QStringList *tokens, QString *error) {
    tokens->clear();
    if (pointer.isEmpty() || !pointer.startsWith(QLatin1Char('/'))) {
        *error = text("a pointer must start with '/'");
        return false;
    }
    const QStringList parts = pointer.mid(1).split(QLatin1Char('/'));
    for (const QString &part : parts) {
        if (part.isEmpty()) {
            *error = text("empty path segment");
            return false;
        }
        QString decoded;
        decoded.reserve(part.size());
        for (int i = 0; i < part.size(); ++i) {
            if (part.at(i) != QLatin1Char('~')) {
                decoded.append(part.at(i));
                continue;
            }
            if (i + 1 >= part.size()) {
                *error = text("a '~' escape is truncated");
                return false;
            }
            const QChar next = part.at(++i);
            if (next == QLatin1Char('0')) decoded.append(QLatin1Char('~'));
            else if (next == QLatin1Char('1')) decoded.append(QLatin1Char('/'));
            else {
                *error = text("'~%1' is not a valid escape").arg(next);
                return false;
            }
        }
        tokens->append(decoded);
    }
    return true;
}

bool isControllerOwnedPath(const QStringList &tokens) {
    return controllerOwnedPaths().contains(tokens);
}

QJsonObject emptyPresetDocument() {
    return QJsonObject{{QStringLiteral("version"), kPresetDocumentVersion},
                       {QStringLiteral("global"), QJsonArray{}},
                       {QStringLiteral("profiles"), QJsonObject{}}};
}

bool parsePresetDocument(const QJsonObject &document, PresetDocument *out,
                         QVector<Diagnostic> *diagnostics) {
    const int mark = diagnostics ? diagnostics->size() : 0;
    const QString source = QStringLiteral("presets");

    const QJsonValue version = document.value(QStringLiteral("version"));
    if (version.isUndefined()) {
        diagnose(diagnostics, "error", source, {},
                 text("The preset document has no \"version\"."));
        return false;
    }
    if (!version.isDouble() || version.toDouble() != version.toInt() ||
        version.toInt() != kPresetDocumentVersion) {
        diagnose(diagnostics, "error", source, {},
                 text("Preset document version %1 is not supported; this build writes version %2.")
                     .arg(version.toVariant().toString())
                     .arg(kPresetDocumentVersion));
        return false;
    }

    PresetDocument parsed;
    QSet<QString> seenIds;
    bool ok = parsePresetArray(document.value(QStringLiteral("global")),
                               QStringLiteral("global"), &parsed.global, &seenIds, diagnostics);

    const QJsonValue profiles = document.value(QStringLiteral("profiles"));
    if (!profiles.isUndefined()) {
        if (!profiles.isObject()) {
            diagnose(diagnostics, "error", source, {},
                     text("\"profiles\" must be an object keyed by profile uid."));
            ok = false;
        } else {
            const QJsonObject byUid = profiles.toObject();
            for (auto entry = byUid.constBegin(); entry != byUid.constEnd(); ++entry) {
                if (entry.key().isEmpty()) {
                    diagnose(diagnostics, "error", source, {},
                             text("\"profiles\" has an empty profile uid."));
                    ok = false;
                    continue;
                }
                QVector<Preset> presets;
                if (!parsePresetArray(entry.value(), entry.key(), &presets, &seenIds, diagnostics))
                    ok = false;
                parsed.profiles.insert(entry.key(), presets);
            }
        }
    }

    // Belt and braces: a helper that forgot to return false still cannot get a
    // document past an error diagnostic.
    if (!ok || (diagnostics && hasError(*diagnostics, mark))) return false;
    *out = parsed;
    return true;
}

QJsonObject presetDocumentToJson(const PresetDocument &document) {
    const auto encodePresets = [](const QVector<Preset> &presets) {
        QJsonArray array;
        for (const Preset &preset : presets) {
            QJsonArray operations;
            for (const Operation &operation : preset.operations) {
                QJsonObject encoded{
                    {QStringLiteral("op"),
                     QString::fromLatin1(kOperationNames[static_cast<int>(operation.kind)])},
                    {QStringLiteral("path"), operation.path}};
                if (!operation.value.isUndefined())
                    encoded.insert(QStringLiteral("value"), operation.value);
                operations.append(encoded);
            }
            array.append(QJsonObject{{QStringLiteral("id"), preset.id},
                                     {QStringLiteral("name"), preset.name},
                                     {QStringLiteral("enabled"), preset.enabled},
                                     {QStringLiteral("operations"), operations}});
        }
        return array;
    };

    QJsonObject profiles;
    for (auto entry = document.profiles.constBegin(); entry != document.profiles.constEnd();
         ++entry)
        profiles.insert(entry.key(), encodePresets(entry.value()));

    return QJsonObject{{QStringLiteral("version"), kPresetDocumentVersion},
                       {QStringLiteral("global"), encodePresets(document.global)},
                       {QStringLiteral("profiles"), profiles}};
}

ComposeResult compose(const ComposeInput &input) {
    ComposeResult result;
    result.logs = input.logs;
    try {
        YAML::Node root = YAML::Load(input.sourceYaml.toStdString());
        if (!root.IsMap()) {
            diagnose(&result.diagnostics, "error", QStringLiteral("source"), {},
                     text("%1 is not a YAML mapping").arg(input.profileName));
            return result;
        }
        result.provenance.append({QStringLiteral("/"), input.sourceLabel});

        bool fatal = false;
        applyPresets(root, input.presets.global, QStringLiteral("global"), &result, &fatal);
        if (!fatal)
            applyPresets(root, input.presets.profiles.value(input.profileUid),
                         QStringLiteral("profile"), &result, &fatal);
        // A partly applied layer has no honest rendering, so there is no
        // candidate to hand back: ok stays false and yaml stays empty, which is
        // what keeps the last usable runtime file where it is.
        if (fatal) return result;
        applyOverrides(root, input.overrides, &result);
        if (!applyDefaults(root, &result)) return result;
        applyControllerFields(root, input.controller, resolveMixedPort(input, &result), &result);

        const std::string rendered = yamlutil::dump(root);
        if (rendered.empty()) {
            diagnose(&result.diagnostics, "error", QStringLiteral("source"), {},
                     text("Could not render %1").arg(input.profileName));
            return result;
        }
        result.yaml = QString::fromStdString(rendered);
        result.ok = true;
        return result;
    } catch (const YAML::Exception &error) {
        result.yaml.clear();
        result.ok = false;
        diagnose(&result.diagnostics, "error", QStringLiteral("source"), {},
                 text("Could not compose %1: %2")
                     .arg(input.profileName, QString::fromStdString(error.what())));
        return result;
    }
}

void registerMetaTypes() {
    qRegisterMetaType<Diagnostic>("core::config::Diagnostic");
    qRegisterMetaType<Provenance>("core::config::Provenance");
    qRegisterMetaType<ComposeResult>("core::config::ComposeResult");
    qRegisterMetaType<QVector<Diagnostic>>("QVector<core::config::Diagnostic>");
    qRegisterMetaType<QVector<Provenance>>("QVector<core::config::Provenance>");
}

}  // namespace core::config
