# Refactor progress ledger

Coordinator-owned. Resume from this file plus `git status` after any context loss;
do not repeat completed analysis.

This file and its siblings live in `.refactor/`, **not** in `docs/`. See the binding
cutover rule below.

Last updated: 2026-09-22 (R5-DOCS).

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
| MihomoBackend (`src/core/backend/`) | `backend-r1` | coordinator | **published** `fac0a58` — superseded |
| MihomoBackend (`src/core/backend/`) | `backend-r2` | coordinator | **published** `8be0211` — amendments A1–A4, after BACKEND-FAKE reported five defects against r1 (four accepted) — superseded |
| MihomoBackend (`src/core/backend/`) | **`backend-r3`** | coordinator | **published, current** `cd50da3` — amendments B1–B4, after an independent audit found the real backend violating two of r2's own rules — [BACKEND_CONTRACT.md](BACKEND_CONTRACT.md) |

`backend-r1`, `r2` and `r3` are one interface, not three: A1–A4 and B1–B4 corrected
and tightened its semantics without adding or reordering a published method.
`BackendIdentity::interfaceRevision` is therefore still **1**, and both contract
suites assert it. The earlier row saying the MihomoBackend contract was "not yet
published" was three revisions out of date.

**Where the revision string is written down, and where it was wrong.** All eight
published headers under `src/core/backend/` cited `backend-r1` until R5-DOCS;
they now cite `backend-r3` and describe the amended semantics (A1 stamping, A2/B2
marked supersession, A3 standard-layout `Endpoint`, A4 generations on
`StopCompleted`/`TunChangeCompleted`, B1 post-bump stop stamping, B3 the re-issue
obligation). `backend_bridge.h` already said r3. Two records still say **r2** and
are not R5-DOCS's to edit:

| File | Says | Should say | Owner |
| --- | --- | --- | --- |
| `src/core/CMakeLists.txt:4` (comment on `clash_backend`) | `backend-r2` | `backend-r3` | whoever holds the CMake lease |
| `tests/architecture/architecture.json` — `clash_backend._role`, `backend-contract-tests._note` | `backend-r2` | `backend-r3` | coordinator (holds the file this wave) |

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

### P3 — the backend seam, the UI migration, the composition root, the journeys

Dispatched as the corrected plan at the end of this file. Build directory per
package, so the evidence column names the tree each was verified in.

| ID | Owner / model | Round | Depends on | Writable paths | Status | Evidence / gaps |
| --- | --- | --- | --- | --- | --- | --- |
| P3-CONTRACT-FIX | worker / Opus | R1 | `backend-r3` | `src/core/mihomo/**`, `src/core/backend/types.h`, `tests/core/mihomo/**` | verified | `07de9d6`. B1 and B2 closed in the real backend; the three mutation-surviving bullets addressed; B4 resolved by growing `backend-real-core` from 1 smoke case to **5** driving the locally built mihomo. `backend-real-contract` measured at 26 functions / 31 invocations. Tree `build-p3fix`. |
| P3-FAKE-PARITY | worker / Opus | R1 | `backend-r3` | `tests/support/backend/**`, `tests/contracts/backend/**` | verified | `07de9d6`. B3 closed: the fake now honours the re-issue obligation, so §10's "the same contract tests" is true rather than asserted. `backend-contract` measured at 28 functions / 39 invocations. Tree `build-p3fake`. |
| P3-BRIDGE | worker / Opus | R1 | `backend-r3` | `src/core/backend/backend_bridge.*`, `tests/contracts/backend/bridge/**` | verified | `07de9d6`, linked into the application by `6e8ccc4`. The QObject bridge the ~66 UI `connect()` sites needed; audit defect 7 closed. `backend-bridge` = 25 cases. Tree `build-p3bridge`. |
| P3-UIDATA | worker / Opus | R2 | P3-BRIDGE | `src/ui/pages/**` and their suites | verified | `88ef6cd`. Also **found the offscreen ordering bug** — see the incident record below. Tree `build-p3uidata`. |
| P3-UISHELL | worker / Opus | R2 | P3-BRIDGE | `src/ui/shell/**`, `src/ui/pages/settings/**` and their suites | verified | `88ef6cd`. All 20 UI include sites of `core/mihomo/**` discharged; the `G2-ui-includes-component-private` exception is frozen at **one** site, the composition root. Tree `build-p3uishell`. |
| P3-LEDGER | worker / Opus | R2 | BASE-ARCH | `tests/architecture/**` | verified | The sealed-module rule: a target may no longer reach `clash_mihomo_impl` through `consumers[].deps` while G2 is open, the site list is re-derived from the evaluated graph on every run, the two zombie sites are gone, and E1 was re-phased P4 → P3. Tree `build-p3ledger`. **One site is still wrong** — see the G2 re-derivation below. |
| P3-MAINWIRE | coordinator | R3 | R1, R2 | `src/main.cpp`, `src/app/composition/**` | verified | `88ef6cd`. The composition root now wires the coordinators; audit defects 10 and 11 closed. Tree `build-p3wire`. |
| P3-WORKFLOWS | worker / Opus | R4 | R3 | `tests/workflows/**` | verified | `e592003`. W01, W03, W04, W05 and the application smoke harness: **5 suites, 11 test functions, 12 invocations**, all `RUN_SERIAL` on the fixed controller port 29097. Found and fixed the **cold-start system-proxy deadlock** — see the incident record. Tree `build-p3wf`. |
| P3-AUDIT (×3) | independent readers / Opus | between R1 and R2 | — | read-only | verified | Three read-only audits produced the verdict recorded at the end of this file and defects 1–12. Trees `build-audit-contract`, `build-audit-gate`, `build-audit-exit`. |

### R5 — the records the wave left wrong

| ID | Owner / model | Writable paths | Status | Evidence / gaps |
| --- | --- | --- | --- | --- |
| R5-DOCS | worker / Opus 5 | `tests/README.md`, `.refactor/**`, **comments only** in `src/core/backend/*.h` | verified | This entry. `tests/README.md` reconciled against `ctest -N` (47 registered); eight headers moved from `backend-r1` to `backend-r3` with the amended semantics described, not just renumbered; this ledger brought current; the `src/integrations/component/**` deferral recorded; G2 re-derived from the evaluated graph. Changed no code: every header hunk is comment-only, verified by diffing with non-comment lines filtered out. Tree `build-r5d`. |
| R5 — E1 discharge | worker / Opus (parallel) | `src/core/profiles/**`, `src/main.cpp`, `src/app/runtime/**`, `src/core/CMakeLists.txt`, `tests/architecture/architecture.json` | verified, **landed during this wave** | The single `core::vergeConfigPath()` call moved out of `clash_profiles` into the composition root as an injected `seedDir`: `ProfileStore` gained `setSeedDir()`/`seedDir()`, the call lives at `src/main.cpp:172`, the `clash_profiles → clash_mihomo_impl` link is gone from `src/core/CMakeLists.txt`, and the `E1-profiles-links-mihomo-impl` exception is deleted. Cost: a second `G2-ui-includes` site in `src/main.cpp`. Its package ID is not recorded here because R5-DOCS was not given it. |

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


## Documented commands — verified end to end, and what running them exposed

Verifying the *documented path* rather than the ad-hoc command found three defects.
An earlier claim that "the Make facade is verified" covered only `help` and `doctor`.

| Defect | Detail |
| --- | --- |
| `make build` failed to link | The dev preset resolved yaml-cpp from `/opt/anaconda3` while Qt came from `/opt/homebrew`: undefined `YAML::FpToString` / `YAML::Emitter::Write`. The ad-hoc command passed `-DCMAKE_PREFIX_PATH=/opt/homebrew` and never hit it. Fixed: the Makefile resolves a prefix (default `brew --prefix`, override with `CMAKE_PREFIX_PATH`); presets stay free of local paths. |
| `make doctor` hid it | It reported "ok yaml-cpp" without naming the copy — exactly how a broken configuration passes a prerequisite check. It now prints `Qt6_DIR` and `yaml-cpp_DIR` and warns on a split prefix. Verified: the warning fires on the broken configuration. |
| The engine carried the wrong provenance | Go walks up from the submodule and stamped the **superproject's** revision and dirty state into the binary — `vcs.revision` recorded clash-qt's commit where mihomo's `ab405bad` belongs, contradicting the manifest and changing the artifact hash on every unrelated commit here. Fixed with `-buildvcs=false`; provenance comes from the ldflags and manifest. Now verified stable across a superproject commit. |
| `make run` set a variable nothing read | It exported `CLASH_QT_CORE_BINARY` but the app only read the `core/binary` setting, so the command silently did not resolve the staged engine. The app now falls back to the variable; an explicit user setting still wins. |

Verified on macOS 26.6.2 arm64: `make doctor`, `make build`, `make test` (22/22, real
preferences byte-identical), `make core` (reproducible at a fixed path), `make package`.

Artifacts: `build/dev/clash-qt.app`, symlink `build/dev/clash-qt`,
engine `build/dev/core/mihomo` + `mihomo-provenance.json`, staged app
`build/dev/stage/clash-qt.app` (135 MB, Qt frameworks/plugins and the privileged
helper included).

### G1 packaging gap — closed

`make package` now stages the engine at `clash-qt.app/Contents/MacOS/mihomo`, beside
the application executable, which is the first place `CoreProcess::discoverBinary()`
looks, so no runtime change was needed. `mihomo-provenance.json` ships in
`Contents/Resources`. The install runs **after** the Qt deployment script, because
macdeployqt rewrites and signs what it finds and the engine is a self-contained Go
binary that must not be processed as a Qt executable. `package` now depends on `core`
as well as `build`: the engine target is deliberately not in ALL, so a package built
without it would have shipped with no managed engine.

Verified: the staged engine runs and reports `v1.19.31`, and its sha256 matches the
provenance manifest exactly.

### OPEN — G1 violation in engine discovery, assigned to MOD-CORE

`CoreProcess::discoverBinary()` falls back, after the bundled path, to
`QStandardPaths::findExecutable("mihomo")` and then to `bundledBinaries()`, which
hardcodes:

```
/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo
/Applications/Clash Verge.app/Contents/MacOS/verge-mihomo-alpha
```

G1 states the managed engine must never silently use PATH, another Clash
installation, or a downloaded binary. **This developer's machine has Clash Verge
installed and running**, so an app without a staged engine would silently supervise
*that* binary while reporting its own provenance.

Required of MOD-CORE, which owns `core_process.cpp`: the managed path resolves the
staged engine or fails with an actionable message. Any discovery of an unrelated
installation becomes an explicit, separately labelled user choice — never a silent
fallback — and whatever is resolved must be reported so provenance cannot be implied.


## Correction — the G2 link ledger understates the debt, and I caused it

**Claimed:** 8 baselined link sites, owner MOD-CORE, removed in P3, with "a ninth
linker fails the check".

**Actual:** **16 test targets** link `clash_mihomo_impl` directly, plus the
application. Of the exception's 8 listed sites, **2 are zombies** — `runtime-tests`
and `data-pages-tests`, targets deleted by the suite partition — and the checker does
not flag a dead site inside a live exception.

**How the gap opened, twice, both mine.** When registering the partitioned suites and
the P3 test targets, I declared `clash_mihomo_impl` in each consumer's `deps` rather
than adding it to the exception's `sites`. The checker honours `consumers[].deps`
*before* consulting exceptions, so those eight became **permanently allowed** — no
owner, no removal phase, invisible to the ratchet. Migration debt was silently
converted into architecture.

**The "ninth linker" claim was incomplete, not false.** Verified directly: adding
`clash_mihomo_impl` to `backup-tests` without declaring it fails with
`ARCH-R1-UNDECLARED-CONSUMER`. The ratchet does bite an *accidental* addition. What it
does not catch is a *deliberate* declaration in `deps`, which is the path I took eight
times. I verified the include ratchet earlier and generalised to links without testing
that path.

**Consequence.** The G2 link clause cannot be closed on the current ledger: it would
report success while seventeen targets still link the component-private implementation.

**Required before P3 closes:** re-derive the site list from the evaluated build graph
rather than maintaining it by hand; remove the zombies; move the eight `deps`
declarations back under the exception with their owner and removal phase; and add a
checker rule that a target may not reach `clash_mihomo_impl` through `deps` while the
G2 exception is open.


## P3 audit verdict, as recorded on 2026-09-21 — the wave was NOT complete

**Historical. Read the "P3 status now" section that follows it for the current
state.** This verdict is kept verbatim because it is the record of what three
independent read-only audits found, and because every package in the R1–R4 plan
below was dispatched against it. Do not edit it to match today.

Three independent read-only audits. Leading with what is not met.

### Acceptance gate (roadmap stage 3)

| Clause | Verdict |
| --- | --- |
| App services run with a fake backend | **met** — four coordinator suites pass against `clash_backend_fake` |
| Real backend passes lifecycle/control contracts | **partially** — 23 cases, but see B1/B4 and the three unprotected bullets |
| W01/W03/W04/W05 exercise assembled behaviour | **NOT MET** — `tests/workflows/` does not exist, none of W01–W05 has any coverage, the `workflow` label is not even reserved |
| Integration (the work package's other half) | **NOT MET** — `main.cpp` includes none of `app/{lifecycle,backup,runtime,composition}` or `core/backend`; `RoutingController` has zero consumers |

### Defects found (all now recorded, one already fixed)

1. **FIXED** — privileged-service mode was dead on HEAD. `NullPrivilegedCoreService`
   with no injection meant `setUseService(true)` always failed and the saved
   preference was silently discarded. Now injected at the composition root with two
   mutually-controlling regression cases.
2. **B1** — `StopCompleted` carries a stale generation on the unconfirmed path; a
   conforming consumer drops it. See `backend-r3`.
3. **B2** — `StopCompleted`/`TunChangeCompleted` cannot express `Superseded`.
4. **B3** — the fake does not implement the re-issue obligation; fake and real
   diverge, so §10's "same contract tests" is false.
5. **B4** — `backend-real-contract` drives the fake core binary, not mihomo.
6. **Three acceptance bullets unprotected by the real suite** (hard cap, managed-vs-
   external stop, probe-cancel ordering) — mutations survive.
7. **G2 contract insufficient to start migration** — `BackendObserver` is a plain
   non-`QObject` while ~66 UI sites use `connect(client_, &MihomoClient::…)`. Nothing
   republishes proxies/rules/traffic/connections/logs/version/config/DNS/providers/
   errors. A QObject bridge must be published first.
8. **G2 link ledger understates debt** — see the correction above.
9. **E1 is mis-phased** — it propagates into `clash-qt` and the checker exempts
   propagated edges, so P3 would close G2's link clause on a false green. Move to P3.
10. **Five unowned behaviours** the wiring would silently drop: the routing-restore
    error message; `beginShutdown()` absent from any quit action (a core would start
    *during* shutdown); startup attach + `openTrafficStream()`; the `warningRaised`
    dialog (omit it and quit hangs forever); and all of inventory group A, for which no
    startup class was ever written.
11. **Blocker neither wiring recipe saw** — `MihomoBackendImpl` privately owns
    `client_`/`process_` with no accessor, while 65 UI sites read `context.client` /
    `context.coreProcess`.
12. Unclaimed: `src/integrations/component/**` never created; `tests/README.md` missing
    10 suites and all W-IDs; headers still cite `backend-r1`; D3's second privileged
    connection still open.

### Decision reversed

The 5000 ms telemetry poll **stays in the composition root**. `fetchVersion()` is the
only site reaching `setConnected(true)` — it is the process's sole liveness probe, and
the re-issue obligation is edge-triggered, never periodic. The contract text already
relies on this poll. Fix only its lifetime: a stack `QTimer`, not a heap one.

### Corrected package plan

**R1, three parallel, all blocking:**
`P3-CONTRACT-FIX` (B1/B2 + the three unprotected bullets + B4, owns `mihomo_backend.*`,
`types.h`, `tests/core/mihomo/**`) · `P3-FAKE-PARITY` (B3 + shared suite, owns
`tests/support/backend/**`, `tests/contracts/backend/**`) · `P3-BRIDGE` (the published
QObject bridge, owns new `src/core/backend/` bridge files).

**R2, two parallel + one:** `P3-UIDATA` (8 sites) · `P3-UISHELL` (10 sites) ·
`P3-LEDGER` (re-derive G2 sites from the build graph, delete zombies, move the eight
`deps` back under the exception, add a rule forbidding `deps` reach while the exception
is open, re-phase E1 to P3).

**R3, coordinator:** `P3-MAINWIRE` — the composition root, carrying all five unowned
behaviours and the five recipe conflicts explicitly.

**R4:** `P3-WORKFLOWS` — W01/W03/W04/W05 plus the application smoke harness, without
which the gate cannot close and the composition root stays untested.


## P3 status now (2026-09-22) — what closed, and what did not

The corrected plan above was executed in full. Taking the audit's own gate table
clause by clause:

| Clause | Verdict on 2026-09-21 | Verdict now | Evidence |
| --- | --- | --- | --- |
| App services run with a fake backend | met | **met** | `shutdown-coordinator` 16, `backup-coordinator` 11, `runtime-coordinator` 13, `routing-controller` 11 cases against `clash_backend_fake` |
| Real backend passes lifecycle/control contracts | partially | **met** | B1/B2 fixed in the real backend, B3 in the fake, B4 resolved; `backend-real-contract` 26 functions / 31 invocations; `backend-real-core` 1 → 5 cases against the locally built mihomo |
| W01/W03/W04/W05 exercise assembled behaviour | **NOT MET** — `tests/workflows/` does not exist | **met** | `tests/workflows/` exists with 5 suites; the `workflow` label is registered and carried by all five; W01 2 cases, W03 2, W04 1, W05 3, `app-smoke` 3 functions / 4 invocations |
| Integration (the work package's other half) | **NOT MET** | **met** | `src/main.cpp` wires `app/{lifecycle,backup,runtime,composition}` and `core/backend`; `RoutingController` is consumed by the shell and by W03 |

The twelve defects: 1 was already fixed when recorded; 2–5 (B1–B4) are closed by
P3-CONTRACT-FIX and P3-FAKE-PARITY; 6 addressed by the same package; 7 closed by
P3-BRIDGE; 10 and 11 closed by P3-MAINWIRE; and of defect 12's four unclaimed
items, three are closed by R5-DOCS (`tests/README.md`, the `backend-r1`
citations, and `src/integrations/component/**`, recorded as a deferral below).

**Still open, with owners:**

| Open item | Owner | Phase |
| --- | --- | --- |
| Defect 8 — the G2 link ledger. Re-derived by P3-LEDGER, but one site is still wrong; see the next section | coordinator (holds `architecture.json`) | P3 |
| ~~Defect 9 / E1 — `clash_profiles → clash_mihomo_impl`~~ | CFG-CORE | **discharged 2026-09-22**, during this wave: the call site moved to the composition root, the CMake link is gone and the exception is deleted |
| Defect 12, fourth item — D3's second privileged connection: `ui/service_settings.cpp` still opens its own `PrivilegedServiceClient` alongside the one the backend owns | MOD-CORE / UI | P3 |
| W02 — subscription update. No journey suite; the only coverage is unit-level | CFG-CORE | **P4** |
| `w03-routing-controls` gets neither `QT_QPA_PLATFORM=offscreen` nor `QT_QUICK_BACKEND=software` although it is named in both lists — the offscreen guard, recurring by scope instead of by order; see the incident record | holder of `tests/CMakeLists.txt` / `tests/workflows/CMakeLists.txt` | P3 |
| `src/integrations/component/**` — the portable module loader | COMPONENT-ABI | **P4**, deferral recorded below |
| Windows and Linux — still zero evidence | — | G5, unchanged since BASELINE |


## Correction — the G2 link ledger, re-derived (supersedes the count above)

The earlier correction in this file ("the G2 link ledger understates the debt,
and I caused it") was right that the hand-maintained list was wrong, and
P3-LEDGER re-derived it. The re-derived list is **still wrong, in the other
direction**, and the coordinator's working figure of "13 direct linkers" is wrong
twice over.

**Derived independently by R5-DOCS** from the evaluated CMake File API graph, the
same source `tests/architecture/arch_check.py` reads, in `build-r5d`:

> **`clash_mihomo_impl` has 14 direct linkers**, not 13.

```
app-smoke-tests                 clash_profiles              provider-tests
arch_probe_clash_mihomo_impl    clash-qt                    w01-first-launch-tests
backend-real-contract-tests     controller-tests            w03-routing-controls-tests
backend-real-core-tests         core-process-tests          w04-restore-tests
engine-discovery-tests                                      w05-recovery-tests
```

They divide three ways, and that is why 14 and 13 were both being said:

* **12 are carried by `G2-consumers-link-component-private`** — everything above
  except `arch_probe_clash_mihomo_impl` and `clash_profiles`.
* **`clash_profiles` is carried by `E1-profiles-links-mihomo-impl`**, a different
  exception. The sealed-module rule accepts it because `sealed_while` names
  `E1-*` as well as `G2-*`, so it is baselined — just not under G2.
* **`arch_probe_clash_mihomo_impl` is exempt** under
  `sealed_modules[].exempt_linkers: ["arch_probe_*"]`. It is the public-header
  probe the checker generates for itself, not a consumer.

**`routing-controls-tests` is not a linker at all.** It is listed as a G2 site
and it should not be. Its direct dependencies in the evaluated graph are
`clash_app_runtime`, `clash_backend_bridge` and `clash_backend_fake`. It reaches
`clash_mihomo_impl` only transitively, through
`clash_app_runtime → clash_profiles → clash_mihomo_impl` — **E1's edge,
propagated**, which `check_targets()`'s `is_propagated()` exempts by design. It
was classified as P4/COMPONENT-ABI debt that a module factory would clear. A
factory will not clear it, because there is nothing to clear: when E1's edge goes,
the reach goes with it, at no cost to this suite.

**Which way it went: E1 was discharged while this was being written.** The
14-linker measurement above was taken at the start of R5-DOCS's work, with the
`clash_profiles → clash_mihomo_impl` link still declared in
`src/core/CMakeLists.txt:75`. The parallel worker finished during the wave:
`ProfileStore` now takes an injected `seedDir`, `core::vergeConfigPath()` is
called from `src/main.cpp:172`, the CMake link is gone, and the
`E1-profiles-links-mihomo-impl` exception has been deleted outright.

**End state, re-measured from the evaluated graph after that landed:**

> **13 direct linkers** — the 14 above, minus `clash_profiles`.
> **12 are G2 sites**; the thirteenth is `arch_probe_clash_mihomo_impl`, exempt.
> **`routing-controls-tests` now reaches `clash_mihomo_impl` by no path at all**,
> direct or transitive: `clash_app_runtime → clash_profiles` no longer leads
> anywhere near it. Its G2 site has been deleted, and `arch-graph` passes.

So the misclassification resolved itself in the only way it could: not by a P4
module factory clearing "debt", but by the removal of somebody else's edge.
That is the point worth keeping. A site listed as one target's P4 debt was in
fact another target's propagated edge, and the ledger could not tell the
difference because it was maintained by hand. Deriving the list from the graph is
what makes that visible; `check_targets()`'s `is_propagated()` exemption is what
made it invisible before.

**What `tests/architecture/architecture.json` should still say** (R5-DOCS does
not hold that file this wave and has not edited it; two of the four items below
were fixed by the E1 worker while R5-DOCS worked, and are marked as such):

1. ~~Delete the `routing-controls-tests` site.~~ **Done** by the E1 worker;
   `G2-consumers-link-component-private` now holds 12 sites, exactly the 12
   non-exempt direct linkers in the graph.
2. **Rewrite that exception's `reason` — still wrong.** It opens "Down from 17
   direct linkers to 8" and still names `routing-controls-tests` alongside
   `clash-qt` as the debt that COMPONENT-ABI's P4 factory clears. Both halves are
   false now: the count is **13** direct linkers (12 carried here, 1
   `arch_probe_clash_mihomo_impl` exempt as the checker's own harness), and
   `routing-controls-tests` is not a linker and is no longer listed. It should
   read: debt that clears with the P4 factory — `clash-qt` and the five workflow
   suites, which construct `MihomoBackendImpl` exactly as the composition root
   does; not debt — `controller-tests`, `provider-tests`, `core-process-tests`,
   `backend-real-contract-tests`, `backend-real-core-tests`,
   `engine-discovery-tests`, whose subject *is* the implementation.
3. **`clash_backend._role` (line 265) and `backend-contract-tests._note`
   (line 612) say `backend-r2`.** The contract is `backend-r3`. Still wrong.
4. **`G2-ui-includes-component-private` now contradicts itself.** Its `reason`
   says "Frozen at one site: a second file reaching for the implementation fails
   the check", and it now has **two** sites — both in `src/main.cpp`, the second
   being `core/mihomo/controller_discovery.h`, added by the E1 discharge. The
   sentence appended to the `reason` explains the addition but the "frozen at one
   site" clause was not updated, so the file states a rule it no longer applies.
   Separately, its `_status` block is stale prose from 2026-09-21: "IN FLIGHT…
   Two workers are migrating the UI… 10 of the 20 sites below no longer match",
   against a `sites` list that now holds two entries and a `reason` that says all
   20 UI sites are discharged. Delete `_status`, or replace it with "the UI
   migration landed in `88ef6cd`; the two remaining sites are both the
   composition root".
5. **`sealed_modules[].sealed_while` still names `E1-*`**, which now matches no
   exception. Harmless — `G2-*` still covers the seal — but it is dead prose in a
   file whose whole value is that it is derived rather than remembered.


## Recorded deferral — `src/integrations/component/**` to P4/COMPONENT-ABI

**Recorded 2026-09-22 by R5-DOCS.** This is the fourth item of audit defect 12,
and it was not merely incomplete: it had no owner, no phase and no note anywhere.
A dropped scope item that nobody wrote down is indistinguishable from one that was
never planned.

**What it is.** The portable component *loader* — the half of the component story
that actually loads a module at runtime. `docs/IMPLEMENTATION_ROADMAP.md:237`
gives it the home `src/integrations/component/`, and
`docs/COM_MODULE_PLAN.md:88` says what it owes: load only selected installed
module artifacts, validate identity and version before activation, report
missing or incompatible components, keep the library loaded while any object,
callback, operation or weak-reference control block can still execute its code,
and drain/cancel/close before unloading rather than inferring an asynchronous
stop from a final `Release`.

**Why it vanished.** `docs/PARALLEL_EXECUTION_PLAN.md:90` puts the loader inside
**MOD-CORE's** declared writable scope, next to `src/core/mihomo/**`. MOD-CORE
delivered the backend and the engine facade; the loader was never created and
nothing flagged the omission, because the package it belonged to closed green on
the work it did do.

**Why it is not MOD-CORE's, and not P3's.** It cannot be: a loader needs a module
factory, a module ABI version handshake and a shared-library artifact to load, and
all three are COMPONENT-ABI's deliverables in P4
(`docs/PARALLEL_EXECUTION_PLAN.md:39`, `:95`). `backend-r3` says the same thing in
its own words — r3 is the semantic contract and explicitly *not* the binary
boundary, and "`clashqt_com` is not yet threaded through it".

> **Deferred to P4, owner COMPONENT-ABI**, alongside the module factory, loading
> and lifetime work and the independent sample consumer. Acceptance is
> `docs/COM_MODULE_PLAN.md`'s: an independently built sample consumer and a fake
> module load, query, operate and close on macOS, Windows and Linux;
> incompatible versions fail predictably. That plan states outright that a
> static-library-only test is insufficient.

**And a fact that makes the deferral concrete.** `clashqt_com` — the portable
object model COMPONENT-BASE delivered and `component-contract` exercises with 30
cases — has, in the entire evaluated build graph, **exactly one consumer: its own
contract test.**

```
clashqt_com -> ['arch_probe_clashqt_com', 'component-contract-tests']
```

(`arch_probe_clashqt_com` is the checker's generated public-header probe, not a
consumer.) Nothing in `src/**` links it. It is a well-tested foundation with
nothing built on it yet, and the loader is the thing that would change that. That
is not a criticism of COMPONENT-BASE, which was sequenced deliberately ahead of
its consumers — but it should be visible in the ledger rather than discovered by
someone grepping the link graph in P4.


## Recorded incident — the offscreen guard that did nothing

**Found by P3-UIDATA while migrating the pages; fixed in `447c381`. Cause: the
coordinator's, from registering the partitioned suites.**

`tests/CMakeLists.txt` applied `QT_QPA_PLATFORM=offscreen` from a block that sat
*above* the suite registrations, so its `if(TEST ...)` guard was simply false for
every suite defined later and the property was never set. Measured against the
committed file: the loop ran at line 108, while `preferences` (126),
`config-generation` (187), `home-page` (217), `logs-page` (274) and the rest of
the page suites were registered after it.

**Twelve suites were unguarded, seven of which construct widgets.** They passed
only because this machine has a display. A headless runner would have failed
them, and `traffic-graph` — which also lost `QT_QUICK_BACKEND=software` — is the
suite a worker had seen flake once in 25 runs.

**Why it is recorded.** A guard that quietly does nothing is worse than no guard:
it converts "we verified this headless" into a sentence nobody can check. The
block now runs at the end of the file, and the comment there says why it must
stay there. The benchmark entries remain deliberately excluded: with their
measurement flag set, forcing offscreen turns their skip into a failure.

**It has recurred, and it is still open.** R5-DOCS found the same failure mode
while reconciling `tests/README.md`, in the one workflow suite that builds
widgets. `w03-routing-controls` is named in **both** lists at the end of
`tests/CMakeLists.txt` and receives **neither** `QT_QPA_PLATFORM=offscreen` nor
`QT_QUICK_BACKEND=software`. Verified from the evaluated tree:

```
tray                 => [CLASH_QT_DATA_DIR=..., QT_QPA_PLATFORM=offscreen]
home-page            => [CLASH_QT_DATA_DIR=..., QT_QPA_PLATFORM=offscreen, QT_QUICK_BACKEND=software]
w03-routing-controls => [CLASH_QT_FAKE_CORE=..., CLASH_QT_DATA_DIR=...]
```

The cause is different from the first occurrence but the shape is identical: the
test is registered in the `tests/workflows/` subdirectory, so
`if(TEST w03-routing-controls)` is false in the parent scope where the block
runs, and CMake emits no warning at configure time. Moving the block to the end
of the file fixed ordering; it does not fix scope. It passes today only because
this machine has a display.

**Owner:** whoever holds `tests/CMakeLists.txt` and `tests/workflows/CMakeLists.txt`.
R5-DOCS holds neither and did not touch them. The fix belongs in the subdirectory,
beside `clash_qt_isolate_test()`, which is already the mechanism the other
subdirectory suites use for exactly this reason.


## Recorded incident — the cold-start system-proxy deadlock

**Found by P3-WORKFLOWS (`e592003`) while writing W03. A real user-facing defect,
not a test artifact.**

On a **fresh data directory** the system proxy was unreachable from every
surface, and the cycle is why:

1. the controller reports the system proxy *unavailable* until a proxy target is set;
2. `SettingsPage::renderSystemProxy()` disables the checkbox while it is unavailable;
3. the checkbox was the only thing that ever set a target.

So a new install could not turn the system proxy on at all, from settings, from
the toolbar or from the tray. **Fix:** `SettingsPage::publishProxyTarget()` is
now called whenever the core's endpoint, ports or bypass list move, not only on a
user toggle (`src/ui/pages/settings/settings_page.cpp:215-227`).

**Why it took a journey to find it.** The case
`aColdStartCanEnableTheSystemProxyFromTheSettingsSurface` was written as an
*expected* failure. Publishing the target on every refresh broke the cycle, the
expected failure became an unexpected pass, and it now asserts the behaviour
directly. It is kept as a named regression case because the deadlock is invisible
to every other test in the tree: each of them supplies a target as part of its
own setup. This is exactly the class of defect the journey lane was added for —
a component that builds, links, passes its unit tests and is unusable when
assembled.


## Recorded incident — privileged-service mode shipped dead

**Recorded here for completeness; it is defect 1 of the P3 audit above, and it
was already fixed in `d697d45` when the audit ran.**

Discharging D2 replaced `CoreProcess`'s default-constructed
`platform::PrivilegedServiceClient` with `NullPrivilegedCoreService`, whose
`isSupported()` returns false. `main.cpp` still constructed `CoreProcess` with no
service argument, so on macOS `setUseService(true)` failed at every call, the
user's saved `core/useService` preference was discarded **silently**, the enable
switch in service settings could never succeed, and `usesPrivilegedService()` was
permanently false. `PrivilegedServiceClientAdapter` existed and was linked;
nothing constructed it.

**It shipped because nothing tested it — 38/38 passed throughout.** Two
mutually-controlling regression cases in `core-process` now pin both halves of
the seam. The residual gap the fix itself named — that nothing proved `main.cpp`
performs the injection at all — was closed afterwards by `app-smoke`'s
`theCompositionRootSelectsServiceModeWhenTheUserSavedIt`, which drives the
shipped binary and distinguishes the two modes from outside the process.


## Gate status — measured 2026-09-22 in `build-r5d`

Configured and built fresh at `build-r5d` (Ninja, Debug, `BUILD_TESTING=ON`,
`CMAKE_PREFIX_PATH=/opt/homebrew`) so that no other worker's build tree was
touched. `ctest -N` registers **47** tests.

| Lane | Selection | Registered | Result |
| --- | --- | --- | --- |
| `make test` | `--label-exclude "native\|privileged\|benchmark\|real-core"` | 44 | **44/44 passed** (final run, 2026-09-22, after the E1 discharge landed) |
| `make test-integration` | `--label-regex "real-core\|integration"` | 15 | not run here: it needs `make core`, and `build-r5d` does not stage the engine. The coordinator's figure is 15/15 |
| `make test-native` | `--label-regex "native\|privileged"` | **0** | both labels are reserved and carried by no registered test. The privileged suites carry `service` |

**A transient failure seen mid-wave, recorded because it is instructive.** For
most of R5-DOCS's work the lane read 43/44, with `arch-graph` failing:

```
ARCH-R5-INCLUDE-IR-COMPONENT-PRIVATE  src/main.cpp:56
  #include core/mihomo/controller_discovery.h
```

`G2-ui-includes-component-private` was frozen at exactly one site — `src/main.cpp`
including `core/mihomo/mihomo_backend.h` — precisely so that a second file, or a
second include, reaching for the component-private implementation fails the
check. The E1 discharge added one. The ratchet did its job, the worker finished,
the site was baselined, and the lane returned to 44/44.

**This is the P2 coordination lesson repeating, and it is worth re-reading:**
separate build directories do **not** isolate concurrent source changes. A
verification run during an open lease measures the lease, not the branch. Every
number in this section is a measurement with a timestamp, not a property of the
branch.

**R5-DOCS changed no code.** The eight `src/core/backend/*.h` files were edited
comment-only; verified by filtering the diff to non-comment lines, which is
empty. `tests/README.md` and `.refactor/**` are not compiled.

**Gate clauses that remain open and are nobody's oversight:**

| Clause | State |
| --- | --- |
| Windows, Linux | Zero evidence. No runner in this environment. Unchanged since BASELINE; G5 stays open |
| Native rendering | Everything still runs offscreen or unattended. The two benchmark cases that would exercise a real GPU display are `benchmark`-labelled and excluded, and skip without their flag |
| W02 | No journey suite. P4/CFG-CORE |
| `src/integrations/component/**` | Deferred to P4/COMPONENT-ABI; see the deferral record |
| D3's second privileged connection | Still open |
| E1 | In flight, not discharged |
| `w03-routing-controls` unguarded against a headless runner | Open. It is the one widget-building workflow suite and it receives no `QT_QPA_PLATFORM`; it passes here only because this machine has a display |


## Fresh-checkout verification (2026-09-22, HEAD `e8dbf60`)

Run exactly as a newcomer would, from `git clone` in a clean directory. The point
of doing it from a clone rather than the working tree is that two defects had
already hidden there: `scripts/build/**` was excluded by an unanchored
`.gitignore` pattern, and `make doctor` failed because `build/` does not exist in
a clone — both invisible to any check run in place.

| Step | Result |
| --- | --- |
| `git clone` | 321 tracked files |
| `make help` | exit 0, 22 lines |
| `make doctor` before setup | fails, names `make setup` |
| `make setup` → `make doctor` | all prerequisites present |
| `make build` | 1 m 13 s, **zero errors and zero warnings** |
| `make test` | **44/44** |
| `make test-integration` | **15/15** |
| `make package` | staged |
| Packaged app, launched isolated | starts and exits cleanly |
| Developer's real preferences | byte-identical throughout |

Engine provenance from the clone: `v1.19.31`, source `ab405bad`, state `clean`,
toolchain `go1.26.5`, and the manifest's sha256 **matches the staged binary**. The
engine in the bundle is provably the one built from the recorded submodule.

## Working practice (user directive, 2026-09-22)

Run as a **sub-agent driven workflow with Opus for every sub-agent**, to keep the
coordinator's context small. Architecture, contract revisions, concurrency
decisions and integration stay in the main thread; analysis, implementation and
verification go to workers with bounded scope and exact writable paths.

Two practices earned their cost repeatedly and should be kept: requiring a worker
to **prove** a claim by inverting the behaviour and confirming the test fails,
and strict single-writer file ownership. Several worker self-reports were wrong;
none of the proofs were.
