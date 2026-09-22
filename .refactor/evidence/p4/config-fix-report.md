# CONFIG AUDIT FIXES — detailed notes (config-fix package)

Opus worker, no sub-agents. Writable repository paths used, and no others:

* `src/core/config/config_composer.{h,cpp}`
* `src/core/profiles/profile_store.{h,cpp}`
* `tests/core/config/{config_composer_test,preset_document_test}.cpp`
* `tests/core/profiles/{preset_store_test,effective_config_preview_test}.cpp`

No build/CMake/Make/preset edits, no `main.cpp`, no `architecture.json`, no ledger
or contract writes, no git operations, no writes to any auditor directory, no
submodule changes. No repository build: everything was compiled and run in
`/tmp/clash-qt-p4.w0Y8Uo/config-fix/snap` (own CMake recipe, own build dir).
No helper, no system proxy/VPN/trust store, no network, no subscriptions, no app
launch. Nothing outside the writable list and this scratch directory was written.

## API changes

None. `ComposeResult`/`Diagnostic`/`Provenance`/`ComposeInput`/`ControllerFields`
field names and types are untouched; `compose`, `parsePresetDocument`,
`presetDocumentToJson`, `emptyPresetDocument`, `isControllerOwnedPath`,
`decodePointer`, `registerMetaTypes` keep their signatures; `ProfileStore`'s
public surface and signals are unchanged. New code is internal
(`PresetFileState`/`readPresetFile` in an anonymous namespace,
`alignedMergeDepth`/`mergeFitsTheGuard`/`MixedPort`/`resolveMixedPort` likewise)
plus a `bool *fatal` parameter on two file-static helpers. P5–P9 extension points
untouched.

## F1 — unusable/missing primary no longer means "you have no presets"

`ProfileStore::loadPresets()` is rewritten around one rule: the empty document is
adopted only when there is nothing anywhere to adopt.

* `readPresetFile()` answers `Absent` / `Unusable` / `Valid`. `Absent` requires
  the path to be neither an existing entry nor a symlink, so a mode-000 file, a
  directory at the path, a dangling symlink and an I/O error are all `Unusable`
  and carry `QFile::errorString()` (or the parse/validation error) as the reason.
* `Unusable` primary → error diagnostic naming `presets.json` and the reason,
  then the last-good copy.
* `Absent` primary with a valid last-good → recovered with a warning that says
  the primary is missing and that the next save restores both copies.
* Neither copy usable → presets already in memory are KEPT (warning), the empty
  document is adopted only when memory holds nothing either.
* First run (both absent, nothing in memory) stays silent: no diagnostics, no
  `errorOccurred`, no file created.
* Nothing in the function writes. A bad primary is left byte-identical, so the
  evidence survives; the next `setPresetDocument()` builds on whatever was
  recovered, which is what closes the auditor's "made permanent by the next save"
  amplification — `QSaveFile` renames over the unreadable primary, so the session
  after that needs no recovery at all and reports nothing.

## F2 — a merge past the recursion guard rejects the candidate

`kMaxMergeDepth` (64) stays. What changed is that truncation is no longer
possible: `mergeFitsTheGuard()` measures, before anything is written, how deep
`deepMerge` would actually recurse — counting only levels where BOTH the target
and the fragment are maps, which is precisely where the guard could stop. A
fragment that would not fit produces an error diagnostic naming the pointer, the
limit and what to do about it, sets the internal `fatal` flag, and `compose()`
returns `ok == false` with empty `yaml`. Later presets/overrides/defaults are not
applied, so no half-composed document reaches the renderer.

Consequences proven rather than assumed: `ProfileStore::buildRuntime()` already
returns an empty path when `!composed.ok`, so `runtime.yaml`, the immutable
`.runtime-*.yaml` and the running core are untouched, and the failure arrives
through `errorOccurred`. The preview reports the same failure with the reason
instead of previewing a document nobody described.

Deliberately NOT done: a depth check in `parsePresetDocument()`. Depth is a
property of composing a document against a profile, not of the format, and the
store-level runtime-preservation behaviour is only observable if the document can
be stored. `preset_document_test` pins that division explicitly.

Unrelated limit found while testing, NOT changed (not this package's file): the
renderer in `src/core/config/yaml_util.cpp` throws past 128 nested nodes, so a
~200-level document fails with "Could not render" regardless of the merge guard.
Test depths stay under it so each failure has one cause.

## F3 — compose() honours a trusted mixed-port override itself

`resolveMixedPort()` runs inside `compose()`: a `mixed-port` in
`ComposeInput::overrides` wins over `ControllerFields::mixedPort` when it is an
integral number in 1..65535 (the same rule `ProfileStore::setRuntimeOverrides()`
enforces), and provenance for `/mixed-port` then says `override` instead of
`controller`. Anything that cannot be a port — string, 0, negative, > 65535,
fractional, bool, null — is refused with a warning at `/mixed-port` and the
application default stands. `applyOverrides()` no longer writes the key at all,
so the value has exactly one writer and one provenance entry.
`ProfileStore::controllerFieldsFor()` now supplies `kMixedPort` and nothing else,
so the store stopped pre-resolving. Legacy parity holds for every value the store
can actually store (verified end to end, and by the non-owned
`config_generation_test`, which asserts an override of 28888 in the generated
file). Presets still cannot touch the port: `/mixed-port` remains on the
controller-owned path list and is refused at parse time and at compose time.

## F4 / F5

F4 (TUN key emission order) not required and not changed.

F5 recorded, not acted on: `prepareRuntime()`/`preparePreview()` still call
`enhancer_->snapshot()` on the owning thread, and `preparePreview()` still reads
the profile there. I did not measure the cost and make no claim about it; no
redesign was invented for an unmeasured number. Nothing in F1–F3 changes that
code path, and no test here asserts anything about its timing.

## Evidence

External snapshot `snap/`: repository copies of `src/core/{config,profiles,
preferences,backups}`, `src/ui/{theme,widgets,pages/profiles,resources}`, the six
stable suites, and read-only copies of the three ProfileStore consumers this
package does not own. Own `CMakeLists.txt`; configured with
`-Dyaml-cpp_DIR=/opt/homebrew/opt/yaml-cpp/lib/cmake/yaml-cpp` (the default
resolution picks an ABI-incompatible anaconda build — a local configuration
issue, not a repository finding).

Final run, `ctest-final2.log`: 10/10 suites pass.

| suite | cases | owned |
| --- | --- | --- |
| config-composer | 39 | yes (+9 new) |
| preset-document | 18 | yes (+1 new) |
| preset-store | 23 | yes (+9 new) |
| preview | 18 | yes (+2 new) |
| chain-snapshot | 7 | no, unchanged |
| backup | 15 | no, unchanged |
| preset-editor (UI) | 12 | no, unchanged |
| config-generation | pass | no, unchanged |
| profile-store | pass | no, unchanged |
| maintenance | pass | no, unchanged |

`diff -q` confirms the seven non-owned sources/suites in the snapshot are
byte-identical to the repository, and the eight owned files are byte-identical
between repository and snapshot after the sweep.

### Inversions (`mutate.py`, `inversions/summary.json`)

Twelve single-behaviour flips, applied to the snapshot only, rebuilt, the
affected suites run, source restored and the restoration verified byte for byte
against the repository. All twelve detected.

| id | denies | detected by |
| --- | --- | --- |
| I1 | an unopenable file is told apart from a missing one | apresetFileThatWillNotOpenIsRecoveredNotTreatedAsAFirstRun |
| I2 | a missing primary is recovered from last-good | amissingPrimaryFileIsRecoveredFromTheLastGoodCopy |
| I3 | loaded presets survive a load that reads nothing | presetsAlreadyLoadedAreKeptWhenNeitherCopyCanBeRead |
| I4 | a recovery is diagnosed out loud | 4 recovery cases, incl. the pre-existing malformed one |
| I5 | a too-deep merge takes the whole candidate down | composer ×2, preset-store ×1, preview ×1 |
| I6 | a too-deep merge is noticed at all | composer ×2, preset-store ×1, preview ×1 |
| I7 | the boundary is exactly kMaxMergeDepth (64 refused, 63 kept) | composer ×2 |
| I8 | depth counts only levels where both sides are maps | adeepFragmentThatMeetsNoMatchingMapIsClonedWhole |
| I9 | compose honours the override itself | composer ×4, preset-store ×1, preview ×1 |
| I10 | an override that cannot be a port is refused | amixedPortOverrideThatIsNotAPortIsRefusedWithAWarning |
| I11 | provenance names whoever chose the port | composer ×1, preview ×1 |
| I12 | the store supplies only the default | anImpossiblePortInTheOverridesFileCannotReachTheEngine |

I12 was NOT DETECTED on the first sweep: pre-resolving in the store is invisible
while the override is valid. The missing case was then written (an out-of-range
port planted in `runtime-overrides.json`, which `setRuntimeOverrides()` would
refuse), and I12 is detected by it.

### Isolation

Every test process ran through `run_check.py`: per-run `CLASH_QT_DATA_DIR` under
the scratch root, `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, and
the native preference plist hashed before and after, logged to
`check-runs.jsonl`. Every run reports
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` before and
after — the initial value — including all inversion runs. Tests manipulate only
their own `QTemporaryDir` fixtures: the two permission cases chmod a file inside
the test's own temporary data directory and restore its mode before asserting;
one further case uses a directory at the preset path so the same failure is
covered with no permission change at all.

### Auditor probes after the fix (measured, not predicted)

The auditor's `probe_test.cpp` was copied read-only into this snapshot and run
unmodified against the fixed sources: **27 passed, 3 failed**
(`ctest-auditprobe.log`), against 4 failures before the fix.

* `anUnreadablePresetFileSilentlyLosesPresetsAndNeverTriesLastGood` — now PASS.
  F1's primary reproducer, proven fixed by the independent probe.
* `composeIgnoresAMixedPortOverrideUnlessTheCallerPreResolvesIt` — now PASS. F3.
* `theStoreStillHonoursAMixedPortRuntimeOverrideEndToEnd` — still PASS: legacy
  parity held while the resolution moved into the composer.
* `aMergeDeeperThanTheGuardIsTruncatedWithoutADiagnostic` — FAILS by design: it
  asserts `QVERIFY(result.ok)` on a document the fix refuses outright.
* `theSilentLossIsThenMadePermanentByTheNextSave` — still FAILS, but no longer
  for the audited reason. The probe takes the recovered document and REPLACES
  `global` with a single new preset (`next["global"] = QJsonArray{new}`) rather
  than appending to it, so the save legitimately writes a document without
  `keepme`; a store that kept it would be ignoring the edit it was handed. The
  property the probe was reaching for — the recovered document is what the next
  save builds on — is proven by `therecoveredDocumentIsWhatTheNextSaveBuildsOn`,
  which appends and then asserts both ids in `presets.json` AND
  `presets.last-good.json`, and by the third session in that case loading clean
  with no diagnostics.
* `theTunDefaultsAreEmittedInADifferentOrderThanHead` — FAILS, unchanged: F4,
  cosmetic, explicitly out of scope for this package.

The auditor's files were not modified; the copy lives under this package's own
snapshot.
