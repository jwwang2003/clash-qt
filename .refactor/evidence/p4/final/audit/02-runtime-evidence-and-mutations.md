# P4 final audit — runtime evidence, mutations, safety record

Candidate `be851f5edf34a25d216df62fcd3d35ab52a08e9b`; qualified clone
`/tmp/clash-qt-p4.w0Y8Uo/fresh-6`. All work below is mine, in
`/tmp/clash-qt-p4.w0Y8Uo/final-audit/**`. No repository or coordinator-build write.

## 1. Artifact-level checks I performed myself (not read from a report)

| Check | Command/evidence | Result |
| --- | --- | --- |
| Module exports exactly one C factory | `nm -gU fresh-6/build/p4/modules/libclash_qt_backend_module.dylib` | 1 symbol: `_clashqt_component_module_entry` |
| App has no private-impl link | `nm -a stage/.../MacOS/clash-qt \| c++filt \| grep -c MihomoBackendImpl\|CoreProcess\|MihomoClient` | **0**; 183 `ModuleBackend`/`ModuleLoader` symbols present |
| Staged module uses bundled deps | `otool -L Contents/Frameworks/libclash_qt_backend_module.dylib` | every Qt/yaml-cpp dep is `@loader_path/...`; no Homebrew/Anaconda path |
| Bundle signature | `codesign --verify --deep --strict clash-qt.app` | OK |
| Engine provenance | `shasum -a 256 Contents/MacOS/mihomo` | `6f53b2e1…5b544`, equals `Contents/Resources/mihomo-provenance.json` (`ab405bad…`, v1.19.31, clean, go1.26.5) |

## 2. Independent SDK consumer (exit criterion 1)

Built by me from `fresh-6/examples/component_consumer` as a standalone project with
`-DCLASH_QT_COMPONENT_INCLUDE_DIR=fresh-6/build/p4/stage/include/clash-qt`
(the INSTALLED public SDK, which contains only `core/component/**` — no backend
or engine headers at all).

`CMakeFiles/component_consumer.dir/link.txt` links `QtCore` + IOKit +
DiskArbitration + UniformTypeIdentifiers + libc++ only — **zero project
libraries** (`grep -c clash_ build.ninja` → 0).

Run (`logs/audit-consumer2.log`, isolated data dir via run_check.py, no helper,
free loopback ports, seeded external-ui so no dashboard download):
`ready at 127.0.0.1:55177` → `stop completed, confirmed=yes` →
`live objects before unload: 0` → `unmappable: yes` → `image unmapped: yes`,
exit 0, engine `UI already exists, skip downloading`. No engine left running.

**Minor finding (boundary, honest):** my FIRST attempt pointed the consumer at the
*staged bundle* module, which carries its own copy of Qt 6.11.1 while my consumer
linked Homebrew's Qt 6.11.1. The handshake **accepted** (the runtime tag encodes
the Qt *build*, not the Qt *image*), the process then loaded two `QtCore` images
(objc duplicate-class warnings) and the core never became ready
(`logs/audit-consumer.log`). This is my configuration error, not a contract
violation — module-r1 requires a matching *shared* Qt and the README says to use
the installed module with the app's Qt — but it is worth recording that
`abi::CheckHandshake` cannot detect the duplicate-image case.

## 3. Packaged app, installation-relative module (exit criterion 5)

I launched `fresh-6/build/p4/stage/clash-qt.app/Contents/MacOS/clash-qt` with
`CLASH_QT_MODULE_PATH`, `CLASH_QT_CONTROLLER`, `CLASH_QT_SECRET`,
`CLASH_QT_CORE_BINARY` **unset**, an isolated `CLASH_QT_DATA_DIR` and an isolated
`CLASH_QT_SERVICE_SOCKET`. `lsof` on the live pid:

- mapped: `…/stage/clash-qt.app/Contents/Frameworks/libclash_qt_backend_module.dylib`
  → the loader resolved the **installation-relative** artifact with no override.
- no `/opt/homebrew`, `Cellar` or `anaconda` image mapped.
- no engine child, no helper socket created at all (so no connection to any
  helper, installed or otherwise).
- SIGTERM terminated it cleanly; no kill -9 needed.

The bundle ships only the `cocoa` platform plugin (an offscreen attempt fails
with "Could not find the Qt platform plugin offscreen"), matching the
coordinator's statement. I did not keep the window open long enough for the app
to save geometry, so I confirmed only that it did **not** write the real store;
I did not positively observe a write into the isolated store.

## 4. Mutation proofs (all on my own external copy, `mutate/src`, never the repo)

Baseline before each: suites green, `diff -rq mutate/src/src fresh-6/src` clean.

### M1 — integration, and a *newly discovered* transport case
`src/core/mihomo/mihomo_client.cpp:124-155`: replaced the same-address session
boundary with the pre-fix `return;` (identical address treated as a no-op).
`backend-real-contract` → **5 failures** (`logs/mut1-ctest.log`):
- `sharedReplacementAtTheSameAddressOpensANewSession(direct-real)` AND `(module-real)`
  — "re-attaching to a replaced engine left the generation where it was"
- `aReplacementAtTheSameAddressCannotClearTheNewSession`
- `aRetiredSessionsStreamIsReplacedRatherThanInherited`
- `aRetiredStreamDropCannotDisconnectTheNewSession`

This proves the newly-covered same-address / retired-stream defect class is
genuinely asserted, **and that the assertion reaches the module-backed row**, not
only the in-process one.

### M2 — boundary
`src/core/component/abi/module_entry.h:157-159`: deleted the
`request.runtimeTag != moduleRuntimeTag` refusal.
`abi-contract` → `theHandshakeRefusesAMismatchedRuntime()` **FAILS**
(44 passed, 1 failed, `logs/mut2-ctest.log`). The shared-runtime clause is live.

### M3 — config critical integration
`src/core/config/config_composer.cpp:234` (`applyPresets`): silently drop
profile-scoped presets. → `config-composer` (aborted),
`preset-store::presetsAreAppliedToTheGeneratedRuntimeConfiguration`,
`effective-config-preview::thePreviewIsTheSameCompositionTheRuntimeWouldGet`
and `onlyTheNewestRequestIsAnswered` all **FAIL** (`logs/mut3-ctest.log`).
The UI preview is wired to the *real* composition, not a synthetic one.

All three restored; `diff -rq` against `fresh-6/src` clean; suites green again
(`logs/restored-ctest.log`, `logs/restored2-ctest.log`).

## 5. Four-way shared set, measured by me

From `logs/mut1-ctest.log`: 17 distinct `shared*` cases × exactly
`direct-fake`/`direct-real`/`module-real`/`module-fake` = **68 rows**, 105 cases
in the suite, **0 skipped, 0 blacklisted**. An unavailable subject is a
`QVERIFY2` failure by construction (backend_real_contract_test.cpp:1780-1784),
not a skip.

## 6. D8 marshalling measurement, reproduced by me

`connection-snapshot-marshal` (benchmark lane) in my own Debug build:
`500-row snapshot, 200 round trips: packed 1440.37 ms (114182 B), naive 1688.54 ms (136336 B)`,
`build=Debug Qt=6.11.1 packed/naive=0.853`. Consistent with the coordinator's
Debug figures and identical byte counts. The case asserts **no** speedup — it
records. Correctness slots live in the routine `connection-snapshot-codec` lane;
every one of the 6 slots is registered in one lane or the other.

## 7. Fresh-clone gates (raw logs, not summaries)

`fresh-6-logs/test-ctest/LastTest.log` and `test-integration-ctest/LastTest.log`:
every QTest `Totals:` line reads `0 failed, 0 skipped`. Earlier at `ac3d8e6` I
independently summed 726 QTest functions passed / 0 failed / 0 skipped across the
54 routine entries. `make test` 54/54, `make test-integration` 20/20,
`make package` exit 0.

I also observed, and verified as genuinely repaired rather than waived:
- `w02-real-core` FAILED at `c3282ca` (`liveProcesses 2, expected 1`). The fix
  (`edb78ec`) does not weaken the count — it adds a readiness ledger keyed by
  generation and gates on `isRestartPending()` + exact `activeConfigPaths()`
  before counting, and adds `QCOMPARE(ready.count(), 2)`.
- `make package` failed twice on macOS deployment/signing (`b3ba171`, `748a600`,
  `be851f5`) before passing. The module is now passed to the deploy tool and the
  nested module/helper are signed before `codesign --verify --deep --strict`.

## 8. SAFETY — full, unvarnished record

> **AMENDED 2026-09-22 (attribution only).** The causal claim at the end of this
> section — that my harness's `QSettings` domain lookup caused the geometry write —
> is **withdrawn as unsupported**; see
> `/tmp/clash-qt-p4.w0Y8Uo/preference-recheck/01-preference-attribution-correction.md`.
> The user has since confirmed: *"Yes, I used a separate clash-qt window."* Hashes,
> key contents, measurements and every other finding in this file are unchanged.
> Inline corrections below are marked **[AMENDED]**.

The installed privileged helper (pid 89247, started 2026-09-21) was never
installed, uninstalled, restarted or connected to **by me** — **[AMENDED]** this
assertion covers *my* audit only; the historically recorded early coordinator smoke
runs that could contact the installed helper for status
(`.refactor/P4_QUALIFICATION.md:96`, notes/01 §H) remain withdrawn-and-documented,
and are not superseded by it. No system proxy, VPN, trust
store or network setting was touched. No real subscription or remote URL. Engine
discovery never reached Clash Verge (my consumer and the app were given explicit
local paths or none).

**The user's preference plist changed during my audit window, and I am reporting it
rather than concealing it.** **[AMENDED]** — the original heading read "I changed
the user's preference plist"; authorship was never established and is corrected
below.

- Baseline at my start: `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` (matched the recorded value).
- Observed after my first `ctest` invocation in my own headless build (16:29):
  `82a0d1e4531f9b55cdcaa986f3d2a37519d3acd34186eff35190f96a5f744f04`.
  **[AMENDED]** "after", not "caused by": a separate user-driven GUI clash-qt
  (pid 57802, `…/build/p4/clash-qt.app/…`, **not** my staged binary and not the
  coordinator's pid 51881) was in use over 16:28:47–16:29:21 and received a Dock
  quit at 16:29:20.934, twelve seconds before my run's log was written.
  That run also produced the suite's own guard failure:
  `sharedFailedValidationLeavesTheRunningConfigurationIntact(module-fake)
   'environment_->realPreferencesUnchanged()' returned FALSE`.
- It drifted a **second** time, to
  `204612a89e487d9f3307438f3f2ca97e60056c66919bcebe113a89f863428ffc`,
  **while nothing of mine was running**, and has been stable since (re-read for the
  amendment: still `204612a8…`, 133 bytes, on-disk mtime **16:35:57**, one key).
  **[AMENDED]** that mtime is 51 s after my last suite exited (16:35:06) and 6½ min
  after pid 57802 exited; `cfprefsd` owns the file and defers flushes, so the mtime
  identifies no writer. This transition is recorded as **unattributed**.

What the evidence says about the cause:
- The file still holds **exactly one key**, `window.geometry`, 66 bytes. Between
  the two states only the trailing bytes differ (`…0453` → `…0451`) — a window
  geometry value, not a new or lost key.
- My build was configured `CLASH_QT_BUILD_APP=OFF`: it contains **no UI at all**,
  so nothing in it can author a geometry value.
- Re-running the accused row alone 3× and the whole `backend-real-contract` suite
  5× afterwards left the hash unchanged and the guard green.
- Product code cannot reach the native domain under isolation:
  `src/core/preferences/preferences.h:3` forbids it and
  `preferences.cpp:45-57` returns an IniFormat QSettings inside
  `CLASH_QT_DATA_DIR`. No `src/**` call site constructs `QSettings("clash-qt","clash-qt")`.
- The **test harness** does, unconditionally, on every scope:
  `tests/support/scoped_environment.cpp:187`
  `return QSettings(QStringLiteral("clash-qt"), QStringLiteral("clash-qt")).fileName();`
  plus `snapshotOf()` reading the same file (`:35-39`, `:86`).

**[AMENDED] Conclusion, corrected.** The original text concluded that opening that
domain let macOS's `cfprefsd` flush its cached geometry over the stale on-disk copy,
i.e. that my harness triggered the write. **That mechanism is a hypothesis, not a
finding, and is withdrawn.** The captured log
(`/tmp/clash-qt-p4.w0Y8Uo/preference-change-process.json`, 110 events, all pid 57802)
contains **no preference-write event by any process**; its one CFPreferences event is
a *read* at 16:28:47.994, and no `cfprefsd` events were captured at all. What is
established is (a) a **confirmed concurrent user GUI session** — the user states
*"Yes, I used a separate clash-qt window"* — in a UI-capable build that can author
`window.geometry`, which my `CLASH_QT_BUILD_APP=OFF` build cannot, and (b) that the
harness *opens* the native domain, which is not the same as causing this write. That
the GUI session performed the write is the best-fitting explanation but was not
directly observed. No user key was added, removed or corrupted, no product code
wrote, and I performed no preference write and no restoration. The consequence for
the guard still matters:

**the before/after plist hash is not a sound invariant on macOS once the domain
has been touched — and the harness touches it on every single test scope.** The
guard's message ("this test wrote into the developer's own preference store")
over-claims: it cannot distinguish a write from a daemon flush, and its own probe
is a plausible trigger. The header already concedes the guard is "best-effort"
and that a delayed flush "can escape it"; what I observed is the same weakness in
the other direction.

The qualification record's line `After: 681784…` is therefore stale for this
machine: the current value is `204612a8…`. **[AMENDED]** the original sentence
continued "and I caused the first of the two transitions" — **withdrawn**. The first
transition is concurrent with a confirmed user GUI session (pid 57802, quit
16:29:20.934); the second is unattributed. The guard detects concurrent hash change,
not causality. Full corrected record:
`/tmp/clash-qt-p4.w0Y8Uo/preference-recheck/01-preference-attribution-correction.md`.
