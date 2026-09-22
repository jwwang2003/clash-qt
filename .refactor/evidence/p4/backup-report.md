# PRESET BACKUP INTEGRATION — registrations and proof notes

Worker: Opus 5 (claude-opus-5). Lease files written: `src/core/backups/backup_store.cpp`,
`tests/core/backup_test.cpp`. Nothing else in the repository was touched; no git
mutations; no repository build.

## Build registrations the coordinator must add

`src/core/CMakeLists.txt`

    target_link_libraries(clash_backups
        PUBLIC  clash_types
        PRIVATE Qt6::Network yaml-cpp::yaml-cpp clash_preferences clash_config_compose)

PRIVATE is correct: `backup_store.h` names no composer type, so the dependency
does not leak to the ~9 consumers of `clash_backups`.

`tests/CMakeLists.txt`

    target_link_libraries(backup-tests PRIVATE Qt6::Test Qt6::Network clash_backups
        clash_preferences clash_config_compose clash_test_support)

The suite calls `core::config::emptyPresetDocument()` and
`core::config::parsePresetDocument()` directly, so it asserts the same
definition of "loadable" the store enforces instead of restating it.

`tests/architecture/architecture.json`

- `libraries.clash_backups.deps`: add `"clash_config_compose"`.
- `consumers.backup-tests.deps`: add `"clash_config_compose"`.
- `transitive_external` needs no change: `clash_config_compose` pulls
  `clash_types` and `clash_yaml`, whose externals (`Qt6::Core`,
  `yaml-cpp::yaml-cpp`) are already listed on `clash_backups`. `Qt6::Concurrent`
  still must not appear, and does not.

## What changed in backup_store.cpp

- The hand-written root list is split: `requiredFiles` (the three that every
  archive has), `presetFiles` (`presets.json`, `presets.last-good.json`),
  `fileRoots = requiredFiles + presetFiles`, `directoryRoots`,
  `roots = fileRoots + directoryRoots`. `safePath()` now accepts any
  `fileRoots` entry, and the "required and parseable" loop in `validate()`
  iterates `requiredFiles` instead of the positional `roots.mid(0, 3)`, which
  would otherwise have silently started requiring `presets.json`.
- `snapshot()` resolves the preset pair the way `ProfileStore::loadPresets()`
  resolves it: stored document if it parses, last-good if it does not, empty
  version-1 document if neither does; a missing last-good copy is filled from
  the resolved document. `snapshot()` is const and cannot emit, so a corrupt
  preset file failing the whole backup would fail with no explanation, and an
  unparseable document copied through would produce an archive `validate()` can
  never accept.
- `validate()` gained the preset block, placed before `restoreLocal()` stages
  anything: a present entry must satisfy `config::parsePresetDocument`, an
  absent one is materialised as the empty version-1 document in the `files` map
  the restore installs from.
- Secrets policy unchanged: the `profiles.json` `secret` strip and the
  `settingAllowed()` allow-list are untouched. A preset aimed at a
  controller-owned path (`/secret`, `/external-controller`, …) is an error
  diagnostic in the parser, so such an archive is refused outright — covered by
  a payload in `rejectsUnloadablePresetDocumentsBeforeTouchingAnyFile`.

## API names used

`core::config::emptyPresetDocument()`, `core::config::parsePresetDocument()`,
`core::config::PresetDocument`, `core::config::Diagnostic`. New file-local
helpers: `emptyPresets()`, `validPresetDocument()`. No public API added or
changed; `BackupStore`'s header is byte-identical.

## External snapshot

`/tmp/clash-qt-p4.w0Y8Uo/backup/tree` — standalone CMake project over copies of
`backup_store.{h,cpp}`, `config_composer.{h,cpp}`, `yaml_util.{h,cpp}`,
`preferences.{h,cpp}`, `scoped_environment.{h,cpp}` and the suite. Configure
needs `-DCMAKE_PREFIX_PATH=/opt/homebrew`: without it CMake picks up
`/opt/anaconda3/lib/libyaml-cpp.0.8.0.dylib`, which fails to link
`YAML::Emitter::Write`. Build directory `out/`; nothing was built in `build/`
or `dev/`.

Every run: `CLASH_QT_DATA_DIR` a fresh `mktemp -d` under this scratch directory,
`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`.
`~/Library/Preferences/com.clash-qt.clash-qt.plist` hashed before and after each
of the six runs below: `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`
every time, unchanged from the initial hash.

## Runs

| run | result |
| --- | --- |
| clean (first) | 15 passed, 0 failed |
| clean (final, after all mutation trees) | 15 passed, 0 failed |

## Mutations (each on its own copy of the tree, never on the repository)

Logs: `proofs/<name>.log`. Harness: `mutate.py`.

| mutation | change | observed |
| --- | --- | --- |
| A-not-a-root | `fileRoots = requiredFiles` — the pre-package state, presets are not a backup path at all | 11 failed, then the run aborted with SIGSEGV in `cancellingPreparedRestoreLeavesCurrentFilesUntouched` (`localBackups().first()` on an empty list, a cascade of the mutation). `safePath()` rejects the entries `snapshot()` produces, so `createLocal()` returns false everywhere |
| B-no-snapshot-inclusion | delete the `snapshot()` resolution block | 2 failed: `archiveWithoutPresetsRestoresTheEmptyDocument` (archive entry `""` vs `{"global":[],"profiles":{},"version":1}`), `snapshotSubstitutesForAnUnusablePresetFile` |
| C-no-validation | drop the `validPresetDocument` check in `validate()` | 1 failed: `rejectsUnloadablePresetDocumentsBeforeTouchingAnyFile` — `restoreLocal` accepted `presets.json = "{ half written"` |
| D-no-legacy-default | `validate()` skips an absent entry instead of inserting the empty document | 1 failed: `archiveWithoutPresetsRestoresTheEmptyDocument` — restoring a pre-preset archive now fails outright (nothing to install, rollback) |
| E-not-walked | `roots = requiredFiles + directoryRoots`, so the pair is a safe path but is never read or installed | 3 failed, including the exact defect the producer/consumer sweep predicted: after restoring a legacy archive, `presets.json` on disk still held the present-day `global-mode` preset instead of the empty document |

Mutation C aborts at the first of the fourteen rejection cases (`QVERIFY`
returns from the function), so the log shows one failure; that all fourteen are
genuinely refused by the unmutated build is carried by the passing assertion
`errors.size() == unusable.size() * 2`.

## Not done / out of scope

- No repository build and no `ctest` run: the lease forbids building repository
  sources, and the CMake dependency above does not exist yet, so `backup-tests`
  in-tree would not link until the coordinator adds it.
- `src/app/backup/**` (the UI-facing coordinator) was read but not touched. It
  touches only signals and the public slots of `BackupStore` — no reference to
  `snapshot()`, `validate()` or any path literal — and the header is unchanged,
  so it needs no edit.
