# P4 configuration contract — config-r1

Coordinator decision, 2026-09-22. Additive to existing ProfileStore APIs.

## Composition

A pure composer in core/config/config_composer.{h,cpp} consumes already-enhanced
YAML and immutable values. No filesystem/network/process calls. The legacy
enhancement adapter snapshots script/merge contents before background execution;
its existing failure/continue semantics and YAML tags are retained.
Precedence: source → legacy chain → global presets in array order → selected
profile presets in array order → existing shallow-map runtime overrides → final
controller-owned fields/defaults. Explicit false, zero/empty and sequence order
are meaningful. No policy/network product is implemented.

Preset document JSON: {"version":1,"global":[Preset],"profiles":{"uid":[Preset]}}.
Preset: {"id":"stable-id","name":"display","enabled":true,"operations":[Op]}.
Op: {"op":"merge|replace|remove|prepend|append","path":"/dns/enable",
"value": <JSON value>}. Paths use RFC6901 escaping, map keys only; prepend/append
are allowed only at /rules, value an array of strings. Merge deeply merges maps
and replaces sequences; replace replaces exactly; remove removes a key. Missing
remove is idempotent, bad parents/type/version/duplicate IDs are diagnostics.
Controller-owned fields always reasserted, with provenance; attempts to overwrite
them diagnosed. Preserve existing mixed-port override handling and TUN defaults.
No untrusted preset can select its own controller, secret or UI directory.

## UI-facing API (ProfileStore, owning thread)

- QJsonObject presetDocument() const;
- bool setPresetDocument(const QJsonObject &document); atomic persist/validate,
  unchanged state on error; emits presetsChanged() on success.
- void requestEffectiveConfigPreview(const QString &profileUid = {}); async
  immutable input; empty means current; stale results suppressed. No runtime
  cancellation, snapshot/preview-file writes, seeding or backend launch.
- signal void presetsChanged();
- signal void effectiveConfigPreviewReady(const core::config::ComposeResult &result);

core/config/config_composer.h publishes in namespace core::config:
Diagnostic { QString severity, source, path, message; };
Provenance { QString path, source; };
ComposeResult { bool ok=false; QString yaml; QVector<Diagnostic> diagnostics;
QVector<Provenance> provenance; QStringList logs; }; Qt metatypes declared.
Worker may add pure ComposeInput fields/functions and internal types, but these
UI-facing fields/names are fixed for this wave.

## Recovery

Reject invalid presets without losing the previous persisted document. Keep a
last-good preset document and recover it with an explicit diagnostic on malformed
on-disk input. Runtime composition failure must never overwrite the last usable
runtime file or emit a successful candidate. Engine-validation recovery remains
RuntimeCoordinator/backend validation's existing live-core preservation, not a
claim that a YAML composer validates mihomo semantics. Tests must distinguish
composition success from engine-confirmed success.
