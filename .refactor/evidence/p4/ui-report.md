# CFG-UI — registration and proof notes

Worker: CFG-UI (claude-opus-5). Round 2, P4. Scratch root
`/tmp/clash-qt-p4.w0Y8Uo/ui`. No repository build, no git mutation, no build
file edited by this worker.

## Files written (repository)

| Path | State |
| --- | --- |
| `src/ui/pages/profiles/preset_editor.{h,cpp}` | new |
| `src/ui/pages/profiles/effective_config_view.{h,cpp}` | new |
| `src/ui/pages/profiles/profiles_page.{h,cpp}` | edited, legacy controls intact |
| `tests/ui/preset_editor_test.cpp` | new, 12 cases |

## Build registration — already done by the coordinator, verified as correct

Checked against the repository tree at the end of this package; nothing here is
a request for a change unless marked.

* `src/ui/pages/sources.cmake` lists `preset_editor.cpp` and
  `effective_config_view.cpp` beside `profiles_page.cpp`. ✔
* `tests/CMakeLists.txt:425-440` registers `preset-editor-tests` with the three
  page sources, `combo_box.cpp`, `theme.cpp`, `formatting.cpp`, `resources.qrc`;
  links `Qt6::Test Qt6::Widgets Qt6::Concurrent clash_profiles
  clash_test_support`; `clash_qt_isolate_test(preset-editor)`; and sets
  `QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software` **in the same
  `set_tests_properties` call that registers the suite** — the durable form the
  ledger's offscreen-guard incident settled on, not the list at the end of the
  file. ✔
* `tests/workflows/CMakeLists.txt:35-37` adds both new sources to the w03
  source list. ✔ W03 builds the real shell, so without them it would not link.
* Link check: `Qt6::Network` is not named on `preset-editor-tests`, and is not
  needed — `clash_profiles` carries it as a PRIVATE dependency of a static
  library, which CMake propagates as a link-only requirement. Confirmed by the
  external build, which links the same way and resolves.

## External verification

Snapshot: `/tmp/clash-qt-p4.w0Y8Uo/ui/snapshot` (copy of `src/` and
`tests/support` plus the suite), standalone `CMakeLists.txt`, build tree
`/tmp/clash-qt-p4.w0Y8Uo/ui/build`. **No double, no fake:** the suite runs
against the real `core::ProfileStore` and the real `core/config/config_composer`
that CFG-CORE landed, built as `snap_profiles`/`snap_config_compose` with the
same dependency shape `src/core/CMakeLists.txt` uses.

`find_package(yaml-cpp)` first resolved to an Anaconda 0.8.0 copy and failed to
link (`YAML::Emitter::Write`); configured with `-DCMAKE_PREFIX_PATH=/opt/homebrew`
it resolves to 0.9.0 and links. Worth knowing if a fresh checkout is ever
configured on this machine without the repo's presets.

Result: **12 passed, 0 failed** (`run4.log`, and again after the inversion run).
`-Wall -Wextra` produces no warning in any of the four files (`build2`).

Isolation, every run: `ScopedEnvironment` per test function
(`CLASH_QT_DATA_DIR` + restored environment), `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`, `cleanup()` asserts
`ScopedEnvironment::realPreferencesUnchanged()`.
`~/Library/Preferences/com.clash-qt.clash-qt.plist` hashed before and after
every single run: 24 logged hashes in `plist-hashes.log` and
`inversions-final.txt`, all
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`, the initial
value. No privileged helper, no system proxy, no network: the only imports are
local fixture files, and the one URL import in the suite is `file:///etc/passwd`,
which the store rejects before any socket is opened.

## Inversions (harness `invert.py`, external copy only)

Each mutation breaks one claim, rebuilds, runs the suite, and is reverted from
`pristine/`. Full log: `inversions-final.txt`.

| # | Mutation | Cases that failed |
| --- | --- | --- |
| I1 | `commit()` never calls `setPresetDocument` | 6 |
| I2 | failed preview paints `result.yaml` over the last good YAML | `aFailedPreviewKeepsTheLastGoodYamlAndSaysSo` |
| I3 | `requestPreview()` also calls `requestRuntimeConfig()` | 3, incl. `aPreviewIsComposedAsynchronouslyAndWritesNothing` |
| I4 | pointer-shape check removed | `invalidOperationInputIsRefusedBeforeItReachesTheStore` |
| I4b | JSON parse check removed | same |
| I5 | `scopeUid()` always global | 3 |
| I6 | `presetsCommitted → requestPreview` connect deleted | `aPresetEditRequestsAFreshPreviewForTheScopeItWasMadeIn` |
| I7 | `importUrl()` no longer calls the store | `theLegacyProfileControlsSurviveTheNewSections` |
| I8 | operation value replaced by null | 2 |
| I9 | list selection no longer moves the preset scope | `theEditorFollowsTheProfileSelectedInTheList` |
| I10 | `scopeChanged → requestPreview` connect deleted | `aPresetEditRequestsAFreshPreviewForTheScopeItWasMadeIn` |

Restored tree after the run: 12 passed, 0 failed.

**I4 is the reason one assertion exists.** In the first inversion round, removing
the editor's own path validation left the suite green: the *store* rejects the
same document, so "nothing was persisted" held either way and the case proved
nothing about the editor. The case now also asserts
`store.lastPresetDiagnostics().isEmpty()` — a store that was never asked has
nothing to say — and both I4 and I4b fail it. Recorded because it is the same
shape as the ledger's "guard that quietly does nothing".

## Contract notes for the coordinator

* Used beyond the frozen UI-facing five: `ProfileStore::lastPresetDiagnostics()`
  (landed by CFG-CORE) for the inline rejection detail. If that accessor moves,
  `PresetEditor::commit()` is the only caller.
* Nothing in the UI calls `generateRuntimeConfig()`, `requestRuntimeConfig()`,
  `setSeedDir()` or any launch path. The preview is the only new store call.
* No `app_context` or composition-root change: `ProfilesPage` already receives
  the store, and the store owns both the document and the preview.
* Uncovered branch, stated rather than claimed: the store-rejection arm of
  `PresetEditor::commit()` (inline diagnostics + reload from disk) is wired and
  compiled but not exercised, because every document the editor can build is one
  the store accepts. Reaching it needs a store-side fault injection the contract
  does not expose.
* W03 now constructs `PresetEditor` and `EffectiveConfigView` with the rest of
  the shell. Construction reads `presetDocument()` only; the first preview is
  requested on `ProfilesPage::showEvent`, composes on a worker and writes
  nothing, so the journey gains one background composition and no artefact.

## P5–P9 extension points

Preserved, not extended: the preview widget reads `ComposeResult` fields only,
so added diagnostic severities, provenance sources or log lines appear without a
UI change; operation kinds come from one list
(`preset_editor.cpp: operationKinds()`); the scope selector is index-based with
the uid in item data, so a third scope is an item, not a redesign.
