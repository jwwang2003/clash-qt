# Refactor progress ledger

Coordinator-owned. Resume from this file plus `git status` after any context loss;
do not repeat completed analysis.

This file and its siblings live in `.refactor/`, **not** in `docs/`. See the binding
cutover rule below.

Last updated: 2026-09-21.

## Binding rule — what `docs/` may contain on `main`

At the DOCS-CUTOVER gate, `docs/` on the merged `main` contains **only the newly
written implementation documentation**. Two separate categories are removed from the
refactor branch by explicit reviewed file list before the merge:

1. **The 13 pre-refactor planning/audit documents** currently in `docs/`
   (`AUDIT.md`, `BUILD_RELEASE_PLAN.md`, `CAPTURE_MODULE_PLAN.md`, `COM_MODULE_PLAN.md`,
   `FEATURE_PARITY.md`, `IMPLEMENTATION_ROADMAP.md`, `MACOS_SERVICE_PROTOCOL.md`,
   `MODULAR_MANAGEMENT_PLAN.md`, `PARALLEL_EXECUTION_PLAN.md`, `PROJECT_GOALS.md`,
   `QCLASH_REFERENCE.md`, `STRUCTURE_REFACTOR_PLAN.md`, `TEST_STRATEGY.md`).
   Their archive is `legacy-v1` (`28ce5f4`), which must be verified retrievable first.

2. **This refactor's own working notes**, the whole of `.refactor/` — the progress
   ledger, published contract revisions, package dispatch records and handoffs.
   These are process artifacts, not product documentation. Their archive is the
   `codex/refactor-v2` commit history, which the normal merge preserves.

They are kept in `.refactor/` rather than `docs/refactor/` precisely so that no
cleanup step has to remember to carve a subfolder out of the documentation set, and
so that an obsolete plan can never be mistaken for current documentation. Nothing in
`.refactor/` may be linked from the final `README.md` or from any `docs/` page.

The replacement documentation set is written fresh against the actual implementation:
`docs/architecture.md`, `docs/build.md`, `docs/development.md`, `docs/testing.md`,
`docs/packaging.md`, `docs/module-api.md`, `docs/migration.md` — consolidated further
if shorter documents suffice. Every command, link, platform claim and branch reference
in them is checked before the merge.

Neither removal happens early. Old documents stay in place on the refactor branch
until foundation acceptance, because the packages still in flight are specified by them.

## Branch and archival identities

| Identity | Commit | Meaning |
| --- | --- | --- |
| `main` | `b50ff92` | Untouched pre-refactor baseline. No push, merge or reset performed. |
| `legacy-v1` | `28ce5f4` | Frozen G4 archival snapshot. Pre-refactor code + all 13 `docs/` documents + `3rdparty/ref/fxcom` + recorded submodule gitlinks. |
| `codex/refactor-v2` | `28ce5f4` → HEAD | Active refactor branch, created at the same snapshot. |

The snapshot was built through a temporary index and `git update-ref`, so the user's
working tree and staged submodule additions were never disturbed. Every file the
snapshot added was verified byte-identical to disk before HEAD moved.

## Recorded source provenance

| Dependency | Commit | Describe | State at snapshot |
| --- | --- | --- | --- |
| `3rdparty/mihomo` | `ab405bad5beeeac8b003bb01f60f134f6df54471` | `v1.19.31` | clean |
| `3rdparty/mitmproxy` | `b506c68108e287104045333ade476d92c39c275e` | `v5.0.0-2961-gb506c6810` | clean |
| `3rdparty/ref/fxcom` | untracked source drop, archived in snapshot | — | read-only design reference, no licence file present |

## Baseline evidence (pre-move)

Host: macOS 26.6.2 (25G83), arm64. CMake 4.4.2, Ninja 1.13.2, AppleClang 17.0.0,
Qt 6.11.1 (Homebrew), yaml-cpp (Homebrew), Go 1.26.5.

- `cmake -S . -B build-baseline -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=/opt/homebrew` — configure OK.
- `cmake --build build-baseline -j 8` — build OK.
- `ctest --test-dir build-baseline --output-on-failure` — **14/14 passed**, 22.2 s, zero skips.

Registered entries: `runtime`, `controller`, `provider`, `backup`, `data-pages`,
`proxy`, `privileged-service`, `privileged-helper`, `privileged-helper-ipc`,
`system-proxy-async`, `tray`, `routing-controls`, `dashboard-async`, `traffic-history`.
Logs: session scratchpad `logs/baseline-{configure,build,ctest}.log`.

Windows and Linux: **no evidence.** No runner available in this environment; those
G5 acceptance gates stay open.

## Environment findings that constrain the plan

| Finding | Consequence |
| --- | --- |
| Two GNU Makes present: `/usr/bin/make` = **3.81** (macOS stock; Apple will not ship GPLv3) and `/opt/homebrew/bin/gmake` = **4.4.1**. `make` on PATH is still 3.81; Homebrew installs 4.4.1 as `gmake`, with an optional `make` shim at `/opt/homebrew/opt/make/libexec/gnubin/make`. | **Decision: GNU Make 3.81 is the supported floor.** The facade delegates to CMake/Ninja/Go and needs no 4.x feature. Verified on 3.81: `else if` chains, `.DEFAULT_GOAL`, `$(abspath)`, `patsubst`, `$(shell)`, `&&` chaining and failing-recipe propagation (exit 2, later lines skipped) all work. Verified absent: `.ONESHELL` (**silently ignored** — each line still runs in its own shell, no diagnostic) and `--output-sync`/`-O` (4.0+). Recipes are therefore written as single `&&`-chained commands, never relying on `.ONESHELL` or `.SHELLFLAGS`. This coincides with the Windows requirement to avoid `/bin/sh`. `make doctor` reports the detected version; the Makefile states 3.81 as the floor so a 4.x-only feature cannot creep in unnoticed. The same probe was then run under 4.4.1: identical results for every construct the facade uses, and the `.ONESHELL` divergence reproduced on one machine (3.81 prints `A=[]`, 4.4.1 prints `A=[1]`, neither warns). Having both versions locally means the facade is **verified under 3.81 and 4.4.1** before it ships. |
| `3rdparty/mihomo` `go.mod` declares `go 1.20`; local Go is 1.26.5 | Actual minimum must be established by building, not read off `go.mod` (G1 requires this). |
| Upstream mihomo Makefile derives `VERSION` from `git branch --show-current`; a submodule is in detached HEAD, so that branch is empty and it falls back to `git describe --tags` | A project-owned Go build wrapper with explicit flags is required; do not rely on upstream target semantics silently. |
| Upstream generic amd64 targets select `GOAMD64=v3` | Project wrapper pins an explicit v1 baseline for x86_64 desktop compatibility. |
| Reference `fxcom` has **no licence file** | Per COM_MODULE_PLAN the portable component is implemented independently from the reviewed design. No reference implementation is copied. |
| Reference `FRESULT` is `uint32_t` while `FSUCCEEDED`/`FFAILED` compare against 0 as if signed | Confirmed broken failure detection. Project `Result` must be signed with explicit success tests, and expected-failure tests must actually fail. |
| Reference base `IID_IObject` is all zeroes | Project uses freshly generated owned interface IDs. |

## G1 local-core build probe (coordinator, verified)

`3rdparty/mihomo` at `ab405bad` builds from local source with no network fallback
and without modifying the submodule (verified clean afterwards):

```
CGO_ENABLED=0 GOOS=darwin GOARCH=arm64 go build -mod=readonly -tags with_gvisor \
  -trimpath -ldflags "-X .../constant.Version=v1.19.31 -X .../constant.BuildTime=<controlled> -w -s -buildid=" \
  -o build-core-probe/mihomo .
```

| Property | Value |
| --- | --- |
| Source commit | `ab405bad5beeeac8b003bb01f60f134f6df54471` (`v1.19.31`), clean |
| Go toolchain | go1.26.5 darwin/arm64 (`go.mod` declares only `go 1.20`; 1.26.5 is the established working toolchain) |
| Build tags | `with_gvisor` |
| Target | darwin/arm64, `CGO_ENABLED=0` |
| Artifact | 56,406,850 bytes, Mach-O 64-bit arm64 |
| sha256 | `1df6ae267087659698f49baf32d10a5761dfb6faffdbc0808f3a4a05039ddbaa` |
| Self-report | `Mihomo Meta v1.19.31 darwin arm64 with go1.26.5` |

`BuildTime` is set to a controlled constant rather than upstream's wall-clock `date`,
so the value is reproducible. Module downloads happen on first build; this is the
`make setup` network step. This probe artifact is throwaway evidence — `cmake/Mihomo.cmake`
will own the real staged build under `build/<preset>/`.

## Contract revisions

| Contract | Revision | Owner | Status |
| --- | --- | --- | --- |
| Component object model (`src/core/component/`) | `component-r1` | coordinator | **published** — [COMPONENT_CONTRACT.md](COMPONENT_CONTRACT.md) |
| MihomoBackend (`src/core/backend/`) | — | coordinator | not yet published |

## Package ledger

Status values: `queued`, `running`, `ready-for-integration`, `verified`, `blocked`.

| ID | Owner / model | Depends on | Writable paths | Status | Evidence / gaps |
| --- | --- | --- | --- | --- | --- |
| BASELINE | coordinator | — | none (read-only) | verified | 14/14 CTest on macOS arm64; Windows/Linux absent |
| G1-PROBE | coordinator | — | `build-core-probe/` (ignored) | verified | local mihomo builds, provenance recorded; darwin/arm64 only |
| ARCHIVE | coordinator | BASELINE | git refs only | verified | `legacy-v1` = `28ce5f4`, content verified against disk |
| PRE-ARCH | worker A / Opus | — | scratchpad only | verified | 729-line inventory; escalated E1/E2, resolved as D1/D2 |
| PRE-TEST | worker B / Opus | — | scratchpad only | verified | 987-line inventory; 3 intrusive seams located with file:line |
| PRE-MOVE | worker C / Opus | — | scratchpad only | verified | 805-line move map; 170 include rows, all applied without deviation |
| MOVE-UI | worker A / Opus | PRE-MOVE | `src/ui/**` | verified | 50 moves, 115 include rows, QRC + QML checks passed |
| MOVE-CORE | worker B / Opus | PRE-MOVE | `src/core/**`, `src/platform/browser_launcher.*`, `src/service/**` | verified | 24 moves, 17 include rows, helper byte-identical |
| MOVE-TEST | worker C / Opus | PRE-MOVE | `tests/**` | verified | 12 moves, 29 include rows, 117 slot signatures preserved |
| P1-STEP2 | coordinator | all MOVE-* | `CMakeLists.txt`, all manifests, `README.md` | verified | configure+build clean; 131 passed / 0 failed / 2 skipped, identical to baseline |
| P2-BUILD | coordinator | P1 | root+per-dir CMake, `cmake/**`, `scripts/build/**`, `Makefile`, `CMakePresets.json`, `tests/CMakeLists.txt` | verified | 8 libraries build; headless configure registers 10/14; `make core` produces provenance; facade verified on Make 3.81 **and** 4.4.1; `doctor` failure path tested |
| BASE-SEAM | worker A / Opus | P1 | named proxy/browser/dashboard/helper-client files + their suites | verified | 3 seams removed; 14/14; per-suite totals identical to baseline |
| BASE-FIXTURE | worker B / Opus | P1 | `tests/support/**`, `tests/fixtures/**` | verified | scoped env, loopback server, compiled fake core; found and fixed 2 bugs in itself |
| BASE-ARCH | worker C / Opus | P1 | `tests/architecture/**` | verified | 6 rules, 10 self-tests, all failing-when-violated; ratchet verified independently at integration; 17/17 CTest |
| COMPONENT-BASE | worker A / Opus | `component-r1` | `src/core/component/**`, `tests/contracts/component/**` | verified | 32 cases; 21/21 mutations killed; archive has zero undefined symbols |
| SETTINGS-ISOLATION | worker B / Opus | P2 | `src/core/preferences/**`, 15 call sites, `src/main.cpp` (leased) | verified | full run leaves real preferences byte-identical; guard demonstrated failing |

## P1 relocation evidence (verified)

Fresh build directory `build-p1`, macOS 26.6.2 arm64, Qt 6.11.1, CMake 4.4.2, Ninja.

| Check | Result |
| --- | --- |
| Configure | clean |
| Build | clean, no warnings, 173 targets |
| CTest names | all **14**, same names and order as baseline |
| CTest result | 14/14 passed |
| Per-suite case totals vs baseline | **identical** — 131 passed, 0 failed, 2 skipped |
| Line churn in the move commit | 176 additions, 176 deletions; every line an `#include` or a QRC `<file>` body |
| QML | qmldir reads `TrafficGraph 1.0 TrafficGraph.qml`, so `QT_RESOURCE_ALIAS` still applies from the new path; `clash-qt` and `data-pages-tests` each get their own `ClashQt` output directory |
| QRC | `/ui` prefix and both aliases unchanged; `:/ui/chevron-down-*.png` registered in the binary; assets resolve from the new depth |
| macOS helper | `macos_helper.mm` moved byte-identically; present at `Contents/Helpers/`; the bundle-relative path literal untouched |
| Native app launch | `--data-dir` isolated, `--no-autostart`: starts, runs, no crash; stdout **identical** to the pre-refactor build (3 pre-existing `QNativeSocketEngine::write()` warnings in both). Real user preferences untouched; only `instance.lock` written to the isolated directory. |

The 2 skips are pre-existing and are the native-GPU graph cases behind
`CLASH_QT_VERIFY_GRAPH_FRAMES` / `CLASH_QT_MEASURE_FRAMES`. They have never run under
CTest. **No skip is visible at the CTest level today** — surfacing them is P2 work, and
until then a skipped case must not be counted as a pass.

### Explicitly NOT verified after P1

| Gap | Why it is still open |
| --- | --- |
| Normal quit path | The launch check terminated the app with SIGTERM, so the `finishQuit` gate ordering, owned-proxy restoration and "no owned child remains" are **not** exercised. F10/W01 have zero coverage; killing a process is not a quit. |
| Windows, Linux | No runner. Zero evidence. G5 gates stay open. |
| App driving the locally built core | G1 produced a binary and G2's component does not exist yet. No real-core integration path has been run end to end. |
| Skip visibility | `data-pages` reports "Passed" at the CTest level while silently skipping 2 native-GPU cases. |
| Native rendering | Everything ran offscreen or unattended. QML view reaching Ready on a real display, tray menu population and graph rendering are unconfirmed. |

## P2 evidence so far (verified)

| Check | Result |
| --- | --- |
| Static libraries | 8 targets per D1; `clash_types` INTERFACE, `clash_yaml` a leaf so the component never depends on `clash_config` |
| Headless backend | `CLASH_QT_BUILD_APP=OFF` configures with zero Widgets/Quick/Graphs and registers 10 of 14 tests |
| Local-source core | `make core` → `v1.19.31` from `ab405bad`, provenance manifest with toolchain, tags and sha256 |
| Make facade | `help` and `doctor` verified under GNU Make **3.81 and 4.4.1**; `doctor` fails (exit 1) on a missing prerequisite |
| CTest lanes | labels applied; routine lane 14, `ui` lane 4; `real-core`/`native`/`privileged`/`benchmark` reserved and empty |
| Seam removal | all three intrusive seams gone; greps return zero |
| Full suite after integration | **14/14**, per-suite totals identical to the pre-refactor baseline |
| App launch | output identical to the P1 tree |

Root `CMakeLists.txt`: 212 lines → 74.

### Coordination lesson recorded

A coordinator build picked up BASE-SEAM's half-written `dashboard_button.h`: separate
build directories do **not** isolate concurrent source changes. Verification was
restricted to targets outside the active lease and the full run deferred to the
barrier. Future waves: either declare a stable-source build window or verify only
outside active leases.

### Deviation accepted (BASE-SEAM, seam c)

A plain timeout constructor value was specified, but the deadline starts at send
time, so a 20 ms value expires before `runtime_test.cpp` can assert the client is
busy — measured as a real failure, not hypothesised. An injectable `RequestDeadline`
was used instead, defaulting to the internal single-shot timer. Accepted.

## Next ready packages

**P1 is complete and verified.** The relocation barrier is cleared, so dependent
packages may now start against the integrated tree.

**P2 is complete.** P3 is next (three slots): MOD-CORE (MihomoBackend behind the
reviewed contract, severing the 12 UI includes of the component-private header),
MOD-RUNTIME (runtime/routing coordinators), MOD-LIFECYCLE (shutdown/backup
coordinators, and the ownership inversion that removes widget-tree service discovery).
The coordinator must first publish the MihomoBackend contract revision and build the
in-process fake backend under `tests/support/backend/` that all three consume.

Superseded plan for P2 (three slots): BASE-FIXTURE (scoped settings/environment fixtures, loopback servers,
portable fake-core executable), BASE-SEAM (replace the three intrusive seams that
PRE-TEST located with injected dependencies), BASE-ARCH (dependency checker plus its own
positive/negative self-tests, carrying D2's baselined exception pre-registered).

Coordinator in parallel: extract the reusable static libraries per D1, add the Make
facade and `CMakePresets.json`, and `cmake/Mihomo.cmake` for the local-source core build.
COMPONENT-BASE takes a freed P2 slot against contract `component-r1` before any P3
consumer starts.

## Resolved correction — commit `096addf` had a mismatched message

**What happened.** `096addf` is messaged as the D1–D6 decisions document but its tree
also contains ~90 in-flight `R100` renames belonging to MOVE-UI, MOVE-CORE and
MOVE-TEST. Cause: workers relocate with `git mv`, which *stages*. The index is shared
across the whole checkout, so a bare `git commit` by the coordinator swept their
staged renames in even though only `.refactor/DECISIONS.md` was explicitly `git add`ed.

**Impact: no work lost.** Every rename is `R100` (byte-identical content) and the
workers' unstaged in-file include rewrites are untouched in the working tree. The
damage is purely that one commit's message does not describe its contents.

**Not repaired immediately, deliberately.** `git reset --soft` / `--amend` would both
mutate the index while three workers are concurrently running `git mv` against it.
Repairing now risks a genuine race and real loss; the mismatched message risks nothing.

**Repaired at BARRIER 1**, exactly as planned. `git reset --soft 2a6f3ed` (HEAD only;
index and working tree untouched), then three honest commits: `b586739` decisions
document (2 files, via explicit pathspec), `522e503` phase-1 relocation, `3144720`
CMake/manifest reconciliation. `096addf` no longer exists on the branch. Nothing lost.

**Rule adopted:** while any worker holds a lease, the coordinator commits only with an
explicit pathspec (`git commit -- <path>`), never a bare `git commit`.


## Resolved correction — `.gitignore` silently excluded `scripts/`

**What happened.** `.gitignore` carried the unanchored pattern `build*/`, which matches
a directory named `build` at **any** depth — including `scripts/build/`. Commit
`c9e66a5` reported adding the local-core build wrapper and the `doctor` probe, but
`git add -- scripts` silently did nothing and neither file entered the repository,
while `cmake/Mihomo.cmake` referenced one of them. A fresh clone would have failed
both `make core` and `make doctor`.

**Why it was not caught earlier.** Every verification ran in the working tree, where
the files exist. Nothing exercised a pristine checkout, so the gap was invisible to
build and test evidence alike.

**Fix.** Patterns anchored to the repository root (`/build*/`, `/.cache/`). Verified
that all ten build directories are still ignored and that nothing else was hidden —
only `.DS_Store` files, which is intended.

**Guard added.** A fresh `git clone` of the branch is now configured from scratch as
part of integration, not just the working tree. First run: clone contains
`scripts/build/`, and `cmake -DCLASH_QT_BUILD_APP=OFF` configures cleanly.


## BLOCKING FINDING — the test suite writes to the developer's real macOS preferences

**Proven, not inferred.**

1. `~/Library/Preferences/com.clash-qt.clash-qt.plist` was modified at **10:47:40**,
   inside the window of the 17/17 CTest run that finished at 10:48:00.
2. It contains `backup.password = do-not-export`. That literal is written by exactly
   one place in the repository: `tests/core/backup_test.cpp:84`.
3. It also holds `core.useService`, `startup.startCore`, `dashboard.browser`,
   `window.geometry`, `proxies.sort` and four `hotkeys.*` keys — the same keys the
   application itself uses, so test and real values are indistinguishable in it.

**This violates the test strategy's own rule** that a normal test run must not alter
developer state. It is **pre-existing**, not introduced by the refactor.

**Mechanism — partly established.** 14 production sites construct
`QSettings("clash-qt", "clash-qt")` inline. On macOS that resolves to the native
CFPreferences store. `src/main.cpp:68-70` calls `setDefaultFormat(IniFormat)` and
`setPath(IniFormat, UserScope, …)` under `--data-dir`, and `setPath` has no effect on
the native format. BASE-FIXTURE reports the org/app constructor ignores
`setDefaultFormat` too; that specific claim is **not yet independently confirmed**.

**An inconclusive probe, recorded so it is not mistaken for evidence.** Running each
suite with `HOME` redirected left the real plist untouched and produced no file in the
redirected home. That is *not* proof of safety: CFPreferences flushes asynchronously
through `cfprefsd`, and the temporary home was removed before any flush could land.
Treat `HOME` redirection as unverified, not as a safe harness.

**Correction to an earlier claim.** After the first application launch this session the
ledger recorded "real user preferences untouched". That check compared the *list of
filenames* in `~/Library/Preferences`, not contents or modification times, and was
therefore too weak to support the claim.

**Fix scope.** A production change: a single settings accessor that honours
`CLASH_QT_DATA_DIR` with an explicit format, replacing all 14 inline constructions.
Needs its own package; it is not a test-only fix and must not be smuggled into an
unrelated one. Until it lands, treat every full-suite run as writing to the developer's
preferences.


## P2 closed

`ctest` **22/22** on macOS arm64, and a full run now leaves
`~/Library/Preferences/com.clash-qt.clash-qt.plist` byte-identical by sha256 with no
test sentinel present. That is the acceptance criterion the settings defect needed.

### Settings isolation — mechanism established, not assumed

`setDefaultFormat()` applies only to the `QSettings(QObject*)` and
`QSettings(Scope, QObject*)` constructors. On Darwin `QSettings(organization,
application)` is hard-wired to `NativeFormat`, and `setPath` on `NativeFormat` is a
no-op. `main.cpp`'s bootstrap therefore never isolated anything. 15 call sites (not
the 14 first counted) now route through `core::preferences`, a leaf beside
`clash_yaml`. With `CLASH_QT_DATA_DIR` unset the accessor returns literally
`QSettings(organization, application)`, so **existing users keep their settings** —
switching the default to Ini would have orphaned every current user's preferences.

### Coordinator error during integration, recorded

While integrating, the coordinator changed `ScopedEnvironment::productionSettingsFilePath()`
to call the new accessor. That conflated two distinct concepts — *the native store
that must stay untouched* versus *where production currently resolves* — and broke
three passing tests. The change was **not required by the fix**; it was opportunistic
cleanup during integration. Reverted rather than patched further.

**Remaining, with an owner:** `ScopedEnvironment::productionSettingsAreIsolated()`
compares against `settingsDir()` (`<root>/settings`) while the accessor lays out
`<root>/data/settings/<org>/<app>.ini`, so the predicate no longer describes
production. Its sibling `realPreferencesUnchanged()` is correct and is what the
guards actually use. Owner: BASE-FIXTURE's scope, to be reconciled with the accessor
in a bounded follow-up — not during an unrelated integration.

### Known flake, pre-existing

`system-proxy-async` → `slowOperationDoesNotBlockGuiAndRefreshesDeduplicate` asserts
`heartbeats >= 3` and fails under parallel load. 5/5 standalone, and the pre-refactor
baseline build behaves identically, so it is not a regression. It is exactly the
timing assumption the test strategy says to replace with an explicit gate.

### Your preferences file

Backed up to `~/clash-qt-preferences-backup-20260921-131458.plist` (9 keys) and the
domain deleted at the user's instruction. It reappeared at 13:28:42 containing only a
66-byte `window.geometry` blob, with `~/Library/Application Support/clash-qt/` changing
at 13:28:46 — a real application launch against the default data directory, not a test
run. No test sentinel is present in it.
