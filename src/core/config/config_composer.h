#pragma once

// Pure configuration composition, contract revision config-r1
// (.refactor/P4_CONFIG_CONTRACT.md).
//
// Everything here is a function of its arguments. No filesystem, no network, no
// process, no QObject, no clock, no environment. That is the whole point: the
// precedence rules below used to be a 100-line stretch in the middle of
// ProfileStore::buildRuntime(), reachable only by generating a file on a worker
// thread, so "what wins, a preset or an override?" could not be asked without
// writing to disk. It can be asked here, in one call, from a test.
//
// The caller supplies already-enhanced YAML. The legacy enhancement chain
// (scripts and merges) still lives in core/config/enhance; it runs *before*
// this, and its snapshotted contents are what it runs on.
//
// Precedence, lowest to highest:
//   source (already through the legacy chain)
//     -> global presets, in array order
//     -> the selected profile's presets, in array order
//     -> runtime overrides (shallow per-top-level-key map merge)
//     -> defaults the controller fills in (TUN, DNS hijack)
//     -> controller-owned fields, always reasserted -- except mixed-port, where
//        a valid runtime override beats the application default
//
// Explicit false, explicit zero, an explicitly empty sequence and the order of
// a sequence are all meaningful values, never "absent".

#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

namespace core::config {

// ---------------------------------------------------------------- UI-facing
//
// These four names and their field names are fixed for this wave by the
// contract; the UI codes against them. Add beside them, do not reshape them.

/// One thing that went wrong, or was worth saying, while composing.
/// `severity` is "error", "warning" or "info"; `source` names the producer
/// ("preset:<id>", "presets", "override", "controller", "source"); `path` is
/// the RFC 6901 pointer it concerns, empty when it concerns no single path.
struct Diagnostic {
    QString severity;
    QString source;
    QString path;
    QString message;
};

/// Which layer last wrote `path`. Only paths some layer above the source
/// actually touched are recorded: a provenance entry per key of a 4000-proxy
/// profile would be noise, and the question being answered is "who changed
/// this?", not "what is in the file?".
struct Provenance {
    QString path;
    QString source;
};

struct ComposeResult {
    bool ok = false;
    QString yaml;
    QVector<Diagnostic> diagnostics;
    QVector<Provenance> provenance;
    QStringList logs;
};

// ------------------------------------------------------------------ presets

enum class OperationKind { Merge, Replace, Remove, Prepend, Append };

/// One edit. `path` is an RFC 6901 pointer over map keys only; `tokens` is that
/// pointer already unescaped, carried alongside so composition never re-parses.
struct Operation {
    OperationKind kind = OperationKind::Merge;
    QString path;
    QStringList tokens;
    QJsonValue value;
};

struct Preset {
    QString id;
    QString name;
    bool enabled = true;
    QVector<Operation> operations;
};

/// The parsed form of the document ProfileStore persists. `profiles` is keyed by
/// profile uid; a uid with no entry simply has no profile presets.
struct PresetDocument {
    QVector<Preset> global;
    QMap<QString, QVector<Preset>> profiles;
};

/// The fields the application owns outright. A preset or an override that aims
/// at one of them is diagnosed and dropped, and the value here is reasserted
/// afterwards regardless -- so an untrusted preset cannot point the controller,
/// the secret or the dashboard directory anywhere of its choosing.
///
/// One exception, and only for the trusted layer: a `mixed-port` in
/// ComposeInput::overrides wins over `mixedPort` below when it is an integer in
/// 1..65535, because the local proxy port is the user's choice. A preset still
/// cannot reach it. Supply the application default here and let composition
/// settle the value; a caller that pre-resolves the override as well gets the
/// same answer, but the provenance then says "controller" where "override" is
/// the truth.
struct ControllerFields {
    QString externalController;
    QString secret;
    /// The default. Overridden only by a valid runtime override (see above).
    int mixedPort = 0;
    QString externalUi;
    QString externalUiUrl;
    bool storeSelected = true;
};

struct ComposeInput {
    /// YAML the legacy chain has already run over.
    QString sourceYaml;
    /// Labels the bottom of the precedence stack in `provenance`.
    QString sourceLabel = QStringLiteral("profile");
    /// Selects which entry of PresetDocument::profiles applies.
    QString profileUid;
    QString profileName;
    PresetDocument presets;
    QJsonObject overrides;
    ControllerFields controller;
    /// Enhancement-chain console output, carried through untouched.
    QStringList logs;
};

/// Composes, or explains why it could not. Never throws; a YAML exception from
/// any layer becomes an error diagnostic and `ok == false`.
///
/// `ok == false` means no candidate configuration exists and `yaml` is empty.
/// `ok == true` means a document was produced -- it does NOT mean the engine
/// will accept it. Nothing here validates mihomo semantics.
///
/// An operation that is refused (a protected path, a non-map parent) is an error
/// diagnostic and the rest still composes: the other operations are decisions of
/// their own. An operation that could only be applied in PART -- today, a merge
/// value nested deeper than the recursion limit -- fails the whole call instead,
/// because a truncated document is not a weaker version of what was asked for.
ComposeResult compose(const ComposeInput &input);

// --------------------------------------------------------------- the document

/// Highest document version this build writes and accepts.
inline constexpr int kPresetDocumentVersion = 1;

/// Parses and fully validates {"version":1,"global":[...],"profiles":{...}}.
/// Returns false and leaves `out` untouched when any error diagnostic is
/// produced, so a caller can reject a whole document without a partial adopt.
/// `diagnostics` is appended to, never cleared.
bool parsePresetDocument(const QJsonObject &document, PresetDocument *out,
                         QVector<Diagnostic> *diagnostics);

/// The round trip of the above. Emits keys in a stable order so a persisted
/// document is byte-stable across saves that change nothing.
QJsonObject presetDocumentToJson(const PresetDocument &document);

/// An empty, valid document. What a first run gets.
QJsonObject emptyPresetDocument();

/// True when `pointer` names a field the application owns (see ControllerFields).
bool isControllerOwnedPath(const QStringList &tokens);

/// RFC 6901: "/a~1b/c~0d" -> {"a/b", "c~d"}. Returns false and sets `error` for
/// a pointer that is empty, does not start with '/', has an empty token, or
/// carries an invalid '~' escape.
bool decodePointer(const QString &pointer, QStringList *tokens, QString *error);

/// Makes ComposeResult and its members usable across a queued connection.
/// Idempotent; call it before connecting to a signal that carries one.
void registerMetaTypes();

}  // namespace core::config

Q_DECLARE_METATYPE(core::config::Diagnostic)
Q_DECLARE_METATYPE(core::config::Provenance)
Q_DECLARE_METATYPE(core::config::ComposeResult)
