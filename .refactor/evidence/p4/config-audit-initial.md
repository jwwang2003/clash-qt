# Independent CONFIG SUBGATE audit — detailed notes

Read-only audit. Writable path used: `/tmp/clash-qt-p4.w0Y8Uo/config-audit/**` only.
No repository writes, no git operations, no subagents, no app launch, no helper,
no network, no subscriptions. Repo sources verified byte-identical to the
external snapshot after the inversion sweep (`diff -q`, three files, IDENTICAL).

## Method

External snapshot: `snap/` — audit-owned copy of `src/core/{config,profiles,
backups,preferences}`, `src/ui/{theme,widgets,pages/profiles}`, the six worker
suites and two audit-written suites. Own `snap/CMakeLists.txt` (modelled on, not
shared with, the cfg/ui scratch recipes). Built out-of-repo at
`snap/build`, `-Dyaml-cpp_DIR=/opt/homebrew/opt/yaml-cpp/lib/cmake/yaml-cpp`
(the default resolution picked anaconda's ABI-incompatible 0.8.0 and failed to
link — audit-local configuration issue, not a repository finding).

Every ctest invocation runs with `CLASH_QT_DATA_DIR` per test, plus
`QT_QPA_PLATFORM=offscreen` and `QT_QUICK_BACKEND=software`.
`plist-hashes.log` records the preference plist before and after every run and
every mutation run: unchanged at
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` throughout.

### Pure-extraction differential

`tests/audit/probe_test.cpp::headPipeline()` transcribes the HEAD `eb7bab1`
`ProfileStore::buildRuntime()` body (overrides loop, TUN defaults, DNS hijack,
controller reassertion, port removal) with the filesystem stripped out. It is
written from `git show eb7bab1:src/core/profiles/profile_store.cpp`, not copied
from the new composer. 17 corpus rows compare it against `config::compose()`
with an empty preset document, canonicalised (map keys sorted, sequence order
preserved, scalar tags kept). All 17 agree. Raw byte comparison disagrees on one
point only, pinned as its own case.

### Inversions

`mutate.py` — 12 single-behaviour flips applied to the snapshot, rebuilt, target
suite run, source restored. All 12 detected. Summary in
`inversions/summary.md`, per-mutation build and ctest logs beside it.

| id | denies | verdict |
| --- | --- | --- |
| M1 | overrides merge shallowly | DETECTED |
| M2 | controller fields reasserted | DETECTED |
| M3 | controller-owned paths refused | DETECTED |
| M4 | any error diagnostic rejects the whole document | DETECTED |
| M5 | a disabled preset does nothing | DETECTED |
| M6 | a preview writes nothing | DETECTED |
| M7 | superseded previews are dropped | DETECTED |
| M8 | last-good recovery on a corrupt file | DETECTED |
| M9 | a rejected document changes nothing | DETECTED |
| M10 | failed composition keeps the previous runtime | DETECTED |
| M11 | presets are in the archive | DETECTED |
| M12 | unloadable preset documents refused before staging | DETECTED |

## Findings

### F1 (high) — unreadable `presets.json` loses every preset, silently, permanently

`src/core/profiles/profile_store.cpp:735`

```cpp
if (!file.open(QIODevice::ReadOnly)) return;  // a first run has no presets
```

`loadPresets()` cannot distinguish "there is no file" from "the file is there and
will not open". Both land on the empty document. The last-good copy at
`profile_store.cpp:758` is only reached on the *parse/validate* failure path
below, so it is never consulted; `presetDiagnostics_` stays empty and no
`errorOccurred()` is emitted.

Evidence — `ProbeTest::anUnreadablePresetFileSilentlyLosesPresetsAndNeverTriesLastGood`:
valid `presets.json` and valid `presets.last-good.json` on disk, `presets.json`
chmod 000, then `store.load()`:

```
FAIL!  : an unreadable presets.json dropped every preset; last-good was on disk
         and valid. diagnostics=0 errorOccurred=0
```

Amplification — `ProbeTest::theSilentLossIsThenMadePermanentByTheNextSave`:
after the silent loss, the user's next edit goes through
`setPresetDocument()`, which writes *both* files (`profile_store.cpp:803` and
`:810`), so the intact last-good copy holding everything is overwritten:

```
FAIL!  : the last-good copy holding the user's presets was overwritten
```

Against P4_CONFIG_CONTRACT config-r1 §Recovery ("Keep a last-good preset document
and recover it with an explicit diagnostic on malformed on-disk input") this is
arguable — an EACCES file is not "malformed". Against the product-level promise
that a user does not lose configuration, the recovery path is **narrower than
claimed**, and this is the gap flagged per the audit brief. Trigger is narrow
(permission change, I/O error, a directory left at the path); it is not a
regression, because presets did not exist at HEAD.

Shape of a fix (not applied): treat "exists but will not open" as the
unusable-document path — fall through to the last-good copy and diagnose.

### F2 (medium) — a merge deeper than `kMaxMergeDepth` truncates with no diagnostic

`src/core/config/config_composer.cpp:18` and `:60-61`

```cpp
constexpr int kMaxMergeDepth = 64;
void deepMerge(YAML::Node target, const YAML::Node &fragment, int depth = 0) {
    if (depth >= kMaxMergeDepth) return;
```

The guard against untrusted recursion is right; the silence is not. A `merge`
fragment nested past 64 is accepted by `parsePresetDocument()` (Qt's JSON parser
allows it), applies as far as the guard, and stops. No `Diagnostic` is produced,
so the composer reports `ok == true` for a document that does not contain what
the preset said.

Evidence — `ProbeTest::aMergeDeeperThanTheGuardIsTruncatedWithoutADiagnostic`,
70-deep fragment over a 70-deep source:

```
FAIL!  : merge truncated to 'shallow' with no diagnostic (0 diagnostics emitted)
```

Shape of a fix: a `warning`/`error` diagnostic at the truncation point, or a
depth check at parse time so the document is rejected at the door like every
other malformed operation.

### F3 (low) — `compose()` alone drops a `mixed-port` runtime override

`src/core/config/config_composer.cpp:213` excludes `mixed-port` from the
"owned by the application; the override is ignored" warning, which reads as a
promise that the override is honoured. It is not honoured by `compose()`:
`applyControllerFields()` (`:281`) unconditionally writes
`ControllerFields::mixedPort`.

Evidence — `ProbeTest::composeIgnoresAMixedPortOverrideUnlessTheCallerPreResolvesIt`
fails: expected 41234 from `ComposeInput::overrides`, got the default 27890.

The product is correct, because `ProfileStore::controllerFieldsFor()`
(`profile_store.cpp:878`) pre-resolves
`runtimeOverrides_.value("mixed-port").toInt(kMixedPort)` exactly as HEAD did.
Proven end to end by `ProbeTest::theStoreStillHonoursAMixedPortRuntimeOverrideEndToEnd`
(PASS: generated runtime carries `mixed-port: 41234`). This is a published-API
hazard for the next caller, not a user-visible defect.

### F4 (low, cosmetic) — TUN defaults emit in a different key order than HEAD

`config_composer.cpp:247-259` fills `enable, auto-route, auto-detect-interface,
stack`; HEAD filled `enable, stack, auto-route, auto-detect-interface`.

Evidence — `ProbeTest::theTunDefaultsAreEmittedInADifferentOrderThanHead` fails
on the byte comparison while the canonicalised comparison passes. YAML map order
is not semantic and mihomo does not care; the practical effect is that
`runtime.yaml` diffs noisily once across the upgrade.

### F5 (low, observation, NOT measured) — snapshot reads moved onto the owning thread

`prepareRuntime()`/`preparePreview()` call `enhancer_->snapshot()`
(`profile_store.cpp:864`, `:908`), and `preparePreview()` additionally reads the
whole profile up to `kMaxProfileBytes` (16 MiB) at `:900`, all on the owning
thread. HEAD read each chain step lazily on the worker
(`config_enhancer.cpp`, `applyChain` ChainItem overload). This is the deliberate
price of the immutable-snapshot requirement and the diff is otherwise purely
additive (`git diff eb7bab1 -- src/core/config/enhance/`), with failure
attribution and continue-on-failure semantics preserved. The existing
`thePreviewDoesNotBlockTheCaller`
(`tests/core/profiles/effective_config_preview_test.cpp:262`) times *script*
execution, not snapshot I/O, so no suite covers this cost. I did not measure it
and make no claim about its size.

## Claims independently confirmed (probe or inversion, not source comments)

* Pure extraction vs HEAD `eb7bab1`: 17/17 corpus rows agree semantically,
  including explicit `false`/`0`/empty sequence, numeric-looking strings keeping
  their string tag, shallow override map merge, non-map override replacement,
  `port`/`socks-port` removal, `profile:` block present / not a map, ordered
  rules, DNS-hijack on and off.
* Explicit JSON `null` survives both `replace` and a deep `merge` as a null node
  rather than being treated as a removal.
* Rule ordering across two presets and both ends: `prepend`/`append` compose to
  `TOP0, TOP1, TOP2, MID, END1`.
* Protected fields hold against four back-door shapes that are *not* on the
  controller-owned path list — `merge /profile`, `replace /profile` with a map,
  `remove /profile`, `replace /profile` with a scalar — plus the controller,
  secret and external-ui reassertion in each case.
* Provenance names every layer that wrote: `global:<id>`, `profile:<id>`
  (including a `remove` that actually removed something), `override`,
  `controller`, `default`.
* Composition failure leaves the previous `runtime.yaml` byte-identical, keeps
  the previous immutable config on disk, returns an empty path and still speaks
  through `errorOccurred`.
* Preview staleness under churn: 12 interleaved `setPresetDocument` +
  `requestEffectiveConfigPreview` pairs yield exactly one delivered result, and
  it is the newest (`mode: m11`); no `runtime.yaml` is created.
* UI wiring against the real store (`ui-probe`, all PASS): a failed preview keeps
  the older YAML and sets `isStale()`; driving `EffectiveConfigView` five times
  leaves the data directory byte-for-byte unchanged and starts no runtime
  generation; a `PresetEditor` commit is readable by a second, unrelated
  `ProfileStore` that only ever read the disk.
* Backup: presets round-trip, pre-preset archives restore the empty document,
  unloadable preset documents are refused before anything is staged — each
  proven by an inversion (M11, M12, M4) rather than by the suite alone.

## Scope not covered

Windows and Linux: no evidence, not run. ABI/private-backend sources were not
compiled and no statement is made about them or about whole-P4 completion. No
P5–P9 policy scope inferred.
