# INDEPENDENT CONFIG RECHECK — detailed notes (config-recheck package)

Opus worker, no sub-agents. Read-only against the repository, prior workers'
scratch and git. The only path written is
`/tmp/clash-qt-p4.w0Y8Uo/config-recheck/**`. No repository build, no app launch,
no privileged helper, no system proxy/VPN/trust store, no network, no real
subscriptions, no submodule change, no git operation.

## What was rechecked, and against what

Subject: the stable sources as committed in `7b47a02`
("fix(config): recover unreadable presets and reject truncated merges").
`snap/src` is this package's own copy, taken from the repository working tree and
verified byte-identical to it (`diff -q`, five files: `config_composer.{h,cpp}`,
`profile_store.{h,cpp}`, `backup_store.cpp`) and byte-identical to the
config-fix package's snapshot (`diff -rq snap/src`, whole tree, only `types.h`
differed before it was copied).

Own recipe: `snap/CMakeLists.txt` (target prefix `rc_`, own suite list; the
fix package's recipe was read, not shared), configured out of repository at
`snap/build` with `-Dyaml-cpp_DIR=/opt/homebrew/opt/yaml-cpp/lib/cmake/yaml-cpp`
(the default resolution picks an ABI-incompatible anaconda build — a local
configuration issue, not a repository finding).

ABI and controller-discovery sources are NOT in the snapshot and were never
compiled. Nothing was built in `build/` or `dev/`.

## Isolation

Every process ran through this package's own `run_check.py`: a fresh
`CLASH_QT_DATA_DIR` per run under `rundata/`, `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`, and
`~/Library/Preferences/com.clash-qt.clash-qt.plist` hashed BEFORE and AFTER the
process, printed to the log and appended to `check-runs.jsonl`. 50 runs
(baseline, per-suite, every mutant run): before == after ==
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`, the initial
value, without exception. The wrapper exits 99 if it ever differs; that never
fired. Permission cases chmod only files inside the case's own temporary data
directory and restore the mode before the directory is cleaned up; one further
case uses a directory at the preset path and one a dangling symlink, so the same
"exists and cannot be read" class is covered with no permission change at all.

## Suites

`logs/ctest-final.log`: **15/15 suites pass**. Four are this package's own.

| suite | cases | origin |
| --- | --- | --- |
| recheck-recovery | 12 | written here (F1) |
| recheck-compose | 23 | written here (F2, F3) |
| recheck-journey | 8 | written here (preview, backup) |
| recheck-ui | 4 | written here (UI) |
| audit-probe | 30 (1 XFAIL) | first auditor's, carried; two cases rewritten |
| config-composer, preset-document, preset-store, preview | — | carried unchanged |
| chain-snapshot, backup, config-generation, profile-store, maintenance, preset-editor | — | carried unchanged |

### The two rewritten probe cases (in this copy only; the auditor's file is untouched)

* `aMergeDeeperThanTheGuardIsTruncatedWithoutADiagnostic` →
  `aMergeDeeperThanTheGuardFailsTheWholeCompositionWithADiagnostic`. The original
  asserted `QVERIFY(result.ok)` on a document the guard had truncated; that is
  the defect, not the contract. Rewritten to assert the refusal, the empty
  `yaml` and the error diagnostic. **The old expectation passing would be the
  regression.**
* `theSilentLossIsThenMadePermanentByTheNextSave` →
  `theRecoveredDocumentIsWhatTheNextEditBuildsOn`. The original built the "next
  edit" by REPLACING `global` with one new preset, so a store honouring the edit
  legitimately wrote a document without `keepme`. The edit now appends, exactly
  as adding a preset in the editor does.
* `theTunDefaultsAreEmittedInADifferentOrderThanHead` is left as an explicit
  `QEXPECT_FAIL` (F4: map-key emission order, no semantic content, declared out
  of scope). XFAIL keeps it visible in the run output rather than deleting the
  observation. Reported as an unresolved cosmetic limit, not as a pass.

## F1 — findings

All cases in `tests/recheck/recovery_recheck_test.cpp`, written here, not copied.

* **Unusable primary, four shapes, all recover the valid last-good copy and say
  so**: mode-000 file (error diagnostic naming `presets.json`, `errorOccurred`,
  and the bad file left byte-identical — loading writes nothing), a missing file
  (warning, and the primary is NOT silently recreated), a directory at the path,
  a dangling symlink. The dangling-link case asserts only "a diagnostic was
  produced", because `QFileInfo::exists()` is false for it and the code routes it
  through the `isSymLink()` clause.
* **No usable file preserves what is already loaded**: a store that has saved
  `inmemory`, then has one copy replaced by a directory and the other by
  non-JSON, keeps `inmemory`, diagnoses, and emits `errorOccurred`.
* **Nothing usable and nothing held is reported, not passed off as a first run**:
  two error diagnostics, one `errorOccurred`, empty document.
* **A genuine first run is silent**: no diagnostics, no signal, no file created,
  and the whole data directory byte-identical afterwards.
* **An APPENDING edit after recovery keeps the recovered presets**: both ids land
  in `presets.json` AND `presets.last-good.json`, and a third session loads clean
  with zero diagnostics.
* **A deliberately REPLACING document replaces**: `replacement` alone in both
  files. This is an edit being honoured and is explicitly not counted as loss.
* **A refused document changes nothing**: whole-directory fingerprint identical.

## F2 — findings

`tests/recheck/compose_recheck_test.cpp`.

* A 70-level aligned merge: `ok == false`, empty `yaml`, exactly one error
  diagnostic naming `/nest` and `64`. Neither `deep` nor `shallow` appears in the
  output — the truncated document is not produced.
* Boundary measured, not assumed: 63 aligned levels compose and the fragment
  lands; 64 is refused.
* The distinction the contract draws holds: a refused operation (`/secret`, built
  past the parser because the parser refuses controller-owned pointers at the
  door) still composes the rest and leaves the controller's secret standing,
  while a truncating one takes the whole candidate down and a later preset in the
  same document is not applied.
* Integrated: with a deep preset stored, `generateRuntimeConfig()` returns an
  empty path, emits one `errorOccurred` with a reason, and `runtime.yaml`, the
  previously generated immutable file and the number of `*.yaml` files in the
  data directory are all unchanged.
* Preview of the same document fails with diagnostics, empty yaml, and a
  byte-identical data directory.

## F3 — findings

* `compose()` alone honours `overrides["mixed-port"] = 41234` and provenance for
  `/mixed-port` says `override`; with no override it says `controller` and the
  default stands.
* **Agreement table (11 rows, data-driven)**: for each of 41234, 1, 65535, 0, -1,
  65536, 70000, 8080.5, `"8080"`, `true`, `null`, the composer's answer and
  `ProfileStore::setRuntimeOverrides()`'s accept/refuse decision are compared in
  the same case. They agree on every row: accepted values reach the YAML with
  `override` provenance and no warning; refused values leave 27890 with
  `controller` provenance and exactly one warning at `/mixed-port`.
* Legacy end-to-end: `setRuntimeOverrides({"mixed-port": 41234})` then
  `generateRuntimeConfig()` yields `mixed-port: 41234` in the generated file.
* An impossible port (70000) planted directly in `runtime-overrides.json` — the
  only way past the setter — is loaded into `runtimeOverrides()` but reaches the
  engine as 27890.
* A preset cannot choose the port in either layer: `setPresetDocument()` refuses
  the document, and `compose()` handed the parsed form directly refuses the
  operation while the override's 41234 still wins.

## Neighbours

* **Preview**: three previews leave a whole-directory fingerprint identical, emit
  no `runtimeConfigReady`, and create no `runtime.yaml`. Eight edits with eight
  requests deliver exactly one answer and it is the eighth edit's (`step: 7`).
* **UI**: `EffectiveConfigView` on a real store shows the real composition — the
  preset's `mode: global`, `external-controller`, `external-ui` under the data
  directory, `mixed-port: 27890`, and `port` removed — none of which is in the
  profile on disk; and looking at it writes nothing. Driving `PresetEditor`
  through `ProfilesPage` (real widgets, real store) produces a document an
  unrelated second store loads from disk and composes into a runtime carrying
  `mode: global`.
* **Backup**: presets saved by a real `ProfileStore` survive `createLocal()` →
  wipe → `restoreLocal()` and are loaded back by a fresh store with zero
  diagnostics and compose into the runtime. A pre-preset archive (both entries
  stripped, checksums kept honest) restores to a directory a store reads as a
  silent first run. An archive carrying `{"version":9,...}` as `presets.json` is
  refused, `aboutToRestore` never fires, one error is emitted, and the WHOLE data
  directory fingerprint is unchanged — "before touching any file" measured
  against every file, not the three the case is about.

## Mutants (`mutate.py`, `inversions/summary.json`)

Fifteen single-behaviour flips applied to this package's snapshot only, rebuilt
there, the suites that should notice run, source restored, and the restoration
verified byte-for-byte against the repository (`restored_identical_to_repository:
true` for all fifteen). **All fifteen detected.**

| id | denies | detected by |
| --- | --- | --- |
| R1 | an unopenable file is told apart from a missing one | recheck-recovery |
| R2 | a missing primary is recovered from last-good | recheck-recovery |
| R3 | loaded presets survive a load that reads nothing | recheck-recovery |
| R4 | a recovery is announced rather than silent | recheck-recovery |
| R5 | a save restores BOTH copies | recheck-recovery |
| R6 | a merge that cannot land in full is refused | recheck-compose + recheck-journey + audit-probe + preset-store + preview |
| R7 | a truncating operation takes the whole candidate down | recheck-compose + recheck-journey + preset-store |
| R8 | the boundary is the guard itself (63 vs 64) | recheck-compose |
| R9 | compose() honours the override itself | recheck-compose + audit-probe + config-composer + preset-store |
| R10 | an override that cannot be a port is refused | recheck-compose + config-composer |
| R11 | provenance names whoever chose the port | recheck-compose + config-composer |
| R12 | only the newest preview is delivered | recheck-journey + preview |
| R13 | a preview writes nothing | recheck-journey + preview + recheck-ui |
| R14 | an edit is persisted, not held in memory | recheck-recovery |
| R15 | presets are in the archive at all | recheck-journey + backup |

Two observations from the mutant logs, recorded because they explain a
non-detection rather than hide one:

* R1 does NOT fail `audit-probe`. Treating an unopenable file as `Absent` still
  reaches the last-good copy, so the auditor's F1 reproducer still passes; what
  changes is the diagnostic (error → warning) and the directory/dangling cases,
  which `recheck-recovery` asserts.
* R14 does NOT fail `recheck-ui` or `preset-editor`. The mutant skips the
  `presets.json` write but the last-good write is unconditional, so the second
  store recovers the same document — the persistence property still holds through
  recovery. `recheck-recovery` catches the mutant on the primary file's contents.

## Limits, unmeasured or out of scope (no evidence claimed)

* F4 (TUN key emission order) is unchanged and still differs from HEAD. Cosmetic,
  declared out of scope, carried as an XFAIL rather than as a pass.
* F5 (snapshot/profile reads moved onto the owning thread) was NOT measured here
  either. No timing claim is made in either direction.
* `src/core/config/yaml_util.cpp` still throws past ~128 nested nodes, so a
  document far deeper than the merge guard fails with "Could not render" instead
  of the merge diagnostic. Every depth used here stays under it so each failure
  has one cause. Not this package's file; unchanged; reported as a known limit.
* Windows and Linux: not run, no evidence.
* ABI, controller discovery, backend parity and whole-P4 completion: out of this
  subgate, not compiled, no statement made.
* Concurrency beyond the preview's supersession counter, and any behaviour under
  a real engine, were not exercised.
