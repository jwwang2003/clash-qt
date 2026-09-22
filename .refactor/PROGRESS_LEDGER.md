# Refactor progress ledger

Coordinator-owned. Resume from this file plus `git status` after any context loss;
do not repeat completed analysis.

This file and its siblings live in `.refactor/`, **not** in `docs/`. See the binding
cutover rule below.

Last updated: 2026-09-22 (P3-RECORDS).

**How to read a status in this file.** Every closed item names the commit that
closed it and something checkable in the tree. Anything written as "still open"
that does not was, at least twice, already fixed — once in each direction. A
record that is wrong gets acted on, so it is checked against the tree, never
against the prose above it.

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
obligation). `backend_bridge.h` already said r3. Two records said **r2** and were
not R5-DOCS's to edit; one of the two has since been corrected:

| File | Says | Should say | Owner | State |
| --- | --- | --- | --- | --- |
| `src/core/CMakeLists.txt:4` (comment on `clash_backend`) | `backend-r2` | `backend-r3` | whoever holds the CMake lease | **still wrong**, verified 2026-09-22 |
| `tests/architecture/architecture.json` — `clash_backend._role`, `backend-contract-tests._note` | ~~`backend-r2`~~ | `backend-r3` | coordinator | **corrected**; both now read `backend-r3` (verified: `grep -n backend-r tests/architecture/architecture.json` returns only r3) |

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
| P3-LEDGER | worker / Opus | R2 | BASE-ARCH | `tests/architecture/**` | verified | The sealed-module rule: a target may no longer reach `clash_mihomo_impl` through `consumers[].deps` while G2 is open, the site list is re-derived from the evaluated graph on every run, the two zombie sites are gone, and E1 was re-phased P4 → P3. Tree `build-p3ledger`. One site was wrong (`routing-controls-tests`, never a linker) and was deleted by the E1 worker; the exception's *shape* was wrong too and was split by P3-RECORDS — see the two G2 corrections below. |
| P3-MAINWIRE | coordinator | R3 | R1, R2 | `src/main.cpp`, `src/app/composition/**` | verified | `88ef6cd`. The composition root now wires the coordinators; audit defects 10 and 11 closed. Tree `build-p3wire`. |
| P3-WORKFLOWS | worker / Opus | R4 | R3 | `tests/workflows/**` | verified | `e592003`. W01, W03, W04, W05 and the application smoke harness: **5 suites, 11 test functions, 12 invocations**, all `RUN_SERIAL` on the fixed controller port 29097. Found and fixed the **cold-start system-proxy deadlock** — see the incident record. Tree `build-p3wf`. |
| P3-AUDIT (×3) | independent readers / Opus | between R1 and R2 | — | read-only | verified | Three read-only audits produced the verdict recorded at the end of this file and defects 1–12. Trees `build-audit-contract`, `build-audit-gate`, `build-audit-exit`. |

### R5 — the records the wave left wrong

| ID | Owner / model | Writable paths | Status | Evidence / gaps |
| --- | --- | --- | --- | --- |
| R5-DOCS | worker / Opus 5 | `tests/README.md`, `.refactor/**`, **comments only** in `src/core/backend/*.h` | verified | This entry. `tests/README.md` reconciled against `ctest -N` (47 registered); eight headers moved from `backend-r1` to `backend-r3` with the amended semantics described, not just renumbered; this ledger brought current; the `src/integrations/component/**` deferral recorded; G2 re-derived from the evaluated graph. Changed no code: every header hunk is comment-only, verified by diffing with non-comment lines filtered out. Tree `build-r5d`. |
| R5 — E1 discharge | worker / Opus (parallel) | `src/core/profiles/**`, `src/main.cpp`, `src/app/runtime/**`, `src/core/CMakeLists.txt`, `tests/architecture/architecture.json` | verified, **landed during this wave** | The single `core::vergeConfigPath()` call moved out of `clash_profiles` into the composition root as an injected `seedDir`: `ProfileStore` gained `setSeedDir()`/`seedDir()`, the call lives at `src/main.cpp:172`, the `clash_profiles → clash_mihomo_impl` link is gone from `src/core/CMakeLists.txt`, and the `E1-profiles-links-mihomo-impl` exception is deleted. Cost: a second `G2-ui-includes` site in `src/main.cpp`. Its package ID is not recorded here because R5-DOCS was not given it. Landed as `ac60175`. |
| P3-RECORDS | worker / Opus 5 | `.refactor/**`, `tests/README.md`, `tests/architecture/architecture.json` | verified | This entry. The records had drifted the *other* way from R5-DOCS's wave — under-reporting progress and contradicting themselves. Two items listed open were already fixed (**D3** in `88ef6cd`, the **w03 offscreen guard** in `ac60175`); `E1` was listed both discharged and "in flight, not discharged" in the same file. `architecture.json`'s G2 link exception was mis-shaped, not just mis-worded, and is split into debt (`P4`) and a permanent allowance (`never`) — see the correction below. The producer-removed/consumer-left defect class is recorded with its six instances and the three that have no detector. Changed no code: this package holds no `src/`, test-source or CMake file. Tree `build-records`; `arch-graph`, `arch-public-headers` and `arch-selftest` all pass, and the ratchet was shown to bite in both directions. |

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
P3-BRIDGE; 8 closed by P3-LEDGER and P3-RECORDS between them; 9 discharged in
`ac60175`; 10 and 11 closed by P3-MAINWIRE. Defect 12's four unclaimed items are
**all four now closed**: three by R5-DOCS (`tests/README.md`, the `backend-r1`
citations, and `src/integrations/component/**`, recorded as a deferral below) and
the fourth — D3's second privileged connection — in `88ef6cd`, which predates
R5-DOCS. **All twelve are closed.** They were not all closed when this paragraph
first said "three of four"; the fourth had been fixed for two commits and nobody
had checked.

**The punch list, with owners.** Struck-through rows are closed; they are kept
because three of them were listed here as open *after* the commit that fixed
them, which is the drift P3-RECORDS was called in to correct. Five rows remain
genuinely open: two are P4, one is G5, and two are new findings from the
producer-removed/consumer-left sweep recorded below.

| Open item | Owner | Phase |
| --- | --- | --- |
| ~~Defect 8 — the G2 link ledger~~ | coordinator (holds `architecture.json`) | **closed 2026-09-22 by P3-RECORDS**: the wrong site was already deleted by the E1 worker; the exception has now been split into the debt that can reach zero and a permanent allowance, and the reason text re-derived from the graph. See the next section |
| ~~Defect 9 / E1 — `clash_profiles → clash_mihomo_impl`~~ | CFG-CORE | **discharged 2026-09-22**, during this wave: the call site moved to the composition root, the CMake link is gone and the exception is deleted |
| ~~Defect 12, fourth item — D3's second privileged connection~~ | MOD-CORE / UI | **discharged in `88ef6cd`**, and this ledger went on listing it as open for two commits afterwards. `src/ui/pages/settings/service_settings.cpp` constructs no `PrivilegedServiceClient`; status arrives on `BackendBridge::privilegedServiceStatus`. Verified: `grep -rn PrivilegedServiceClient src/` returns nothing in `src/ui/**` but two comments in `service_settings.h` recording that it used to. One connection to the privileged socket, the one `CoreProcess` owns |
| W02 — subscription update. No journey suite; the only coverage is unit-level | CFG-CORE | **P4** |
| ~~`w03-routing-controls` gets neither `QT_QPA_PLATFORM=offscreen` nor `QT_QUICK_BACKEND=software`~~ | holder of `tests/workflows/CMakeLists.txt` | **fixed in `ac60175`**: both are set where the test is registered, in the subdirectory. Verified from the configured tree — see the incident record, which is kept |
| `src/integrations/component/**` — the portable module loader | COMPONENT-ABI | **P4**, deferral recorded below |
| **NEW** — `SettingsPage::systemProxyStateChanged` and `systemProxyBusyChanged` are emitted with zero `connect()` calls anywhere in `src/` or `tests/`. `main_window.cpp` connected the first until `88ef6cd`; the second has never had a receiver. Delete both, or give them one | holder of `src/ui/pages/settings/**` | open, unphased. P3-RECORDS holds no `src/` file |
| **NEW** — `make module` builds `clash_mihomo_impl`, `clash_platform` and the Go core, and never touches `clashqt_com`, the component it is named for. The recipe comment is honest; the target name is not | holder of the `Makefile` | open. Rename the target or make it build the component |
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

**Re-measured again by P3-RECORDS in `build-records`, 2026-09-22: unchanged at 13
direct linkers**, 12 non-exempt. What changed is how they are *carried*: those 12
are no longer one exception. Six of them can never be removed, so they now sit in
a separate permanent entry rather than under a `remove_in: P4` that could not be
honoured — see the next section. The total is the same; the promise is not.

So the misclassification resolved itself in the only way it could: not by a P4
module factory clearing "debt", but by the removal of somebody else's edge.
That is the point worth keeping. A site listed as one target's P4 debt was in
fact another target's propagated edge, and the ledger could not tell the
difference because it was maintained by hand. Deriving the list from the graph is
what makes that visible; `check_targets()`'s `is_propagated()` exemption is what
made it invisible before.

**What `tests/architecture/architecture.json` should still say.** R5-DOCS did not
hold that file and listed five items; **all five are now closed** — two by the E1
worker while R5-DOCS wrote, three by **P3-RECORDS** on 2026-09-22, which does hold
it. Kept with the closure noted against each, because the list is the record of
how far a hand-maintained ledger drifted from a graph it claimed to describe.

1. ~~Delete the `routing-controls-tests` site.~~ **Done** by the E1 worker;
   `G2-consumers-link-component-private` then held 12 sites, exactly the 12
   non-exempt direct linkers in the graph.
2. ~~**Rewrite that exception's `reason`.**~~ **Done, by splitting the exception
   rather than only rewording it** — see "Correction — the G2 link exception was
   mis-shaped" below. The `reason` said "only two are debt" while carrying twelve
   sites and a `remove_in: P4`; six of the twelve can never be removed. It is now
   two entries: `G2-consumers-link-component-private` (6 sites, `migration-baseline`,
   `remove_in: P4`) and `G2-impl-subject-suites-link-component-private` (6 sites,
   `architecture`, `remove_in: never`).
3. ~~**`clash_backend._role` and `backend-contract-tests._note` say
   `backend-r2`.**~~ **Done**, by the E1 worker. Verified 2026-09-22: `grep -n
   backend-r tests/architecture/architecture.json` returns `backend-r3` at every
   hit. `src/core/CMakeLists.txt:4` still says `backend-r2` and is not this
   file's to fix.
4. ~~**`G2-ui-includes-component-private` contradicts itself.**~~ **Done by
   P3-RECORDS.** The `reason` said "The single remaining site" and "Frozen at two
   sites" in the same paragraph; it now states two, names both, and says what a
   third would do. `_status` had already been rewritten by the E1 worker from the
   stale "IN FLIGHT… 10 of the 20 sites below no longer match" to an accurate
   two-site description; P3-RECORDS added the two commits that produced them.
5. ~~**`sealed_modules[].sealed_while` still names `E1-*`.**~~ **Done by
   P3-RECORDS**: the pattern is gone, and the seal's `reason` now records that it
   was dropped because the exception it named was deleted — not because the seal
   was relaxed.


## Correction — the G2 link exception was mis-shaped, not merely mis-worded

**Found and fixed by P3-RECORDS, 2026-09-22.** `G2-consumers-link-component-private`
carried twelve sites under one `remove_in: P4`, with a `reason` that said "only two
are debt" and then described six. The count was the smaller error. The structural
one is that **six of the twelve can never be removed**: `controller-tests`,
`provider-tests`, `core-process-tests`, `engine-discovery-tests`,
`backend-real-contract-tests` and `backend-real-core-tests` link
`clash_mihomo_impl` because the implementation *is their subject*. A test of a
component-private implementation must link that implementation. No factory
removes that link; no phase can.

A migration exception with a removal phase makes a promise: this reaches zero, and
here is when. Half of this one could not keep it. Left as it was, the P4 owner
would have inherited six "sites" to discharge that are not debt at all — and the
count would have stopped falling at six while still being printed under "must
shrink to zero".

**Split into two entries**, both verified against the evaluated graph in
`build-records`:

| Entry | Class | `remove_in` | Sites | What they are |
| --- | --- | --- | --- | --- |
| `G2-consumers-link-component-private` | `migration-baseline` | `P4` | 6 | `clash-qt` and the five workflow suites. All six construct `core::MihomoBackendImpl` — the application at `src/main.cpp:131`, the suites through `tests/workflows/workflow_support.h:538`, which is the point of a journey. They clear when COMPONENT-ABI publishes a factory from `clash_backend` |
| `G2-impl-subject-suites-link-component-private` | `architecture` | `never` | 6 | The six suites above whose subject is the implementation. A permanent allowance, with a `reason` that says why it will never be removed |

**What a permanent entry puts in `remove_in`, and why the checker accepts it.**
`arch_check.py`'s `Exceptions.REQUIRED` demands `id`, `kind`, `owner`, `remove_in`
and `reason` on every entry, and validates none of their *values* — `remove_in` is
only ever printed, in the summary and in two violation messages. The value chosen
is the string **`"never"`**, which is not an invention: the checker's own self-test
fixtures under `tests/architecture/selftest/cases/**` already use `"remove_in":
"never"` for entries that are not debt. `class` is `"architecture"` rather than
`"migration-baseline"`, which is what keeps it out of the summary's "migration
baseline (must shrink to zero)" group; `class` defaults to `"architecture"` in the
summary code, so the value is explicit rather than implied.

**The id keeps the `G2-` prefix deliberately.** `sealed_modules[].sealed_while`
matches exceptions **by id glob**, and clause (b) of that seal — every target that
links the sealed module must appear as a baselined site — is the rule that keeps
this list derived from the graph instead of remembered. An id outside `G2-*` would
have removed the six permanent linkers from the seal's cover and silently
reintroduced the exact hole that let eight sites become architecture through
`consumers[].deps`.

**Proved, not assumed** (`build-records`, Ninja/Debug):

* `arch-graph`, `arch-public-headers` and `arch-selftest` all pass with the split
  in place; the summary prints `baselined architecture exceptions (1)` and
  `migration baseline (must shrink to zero) (2)` under separate headings.
* Delete the permanent entry from a copy of the file and the check **fails with 12
  violations** — six `ARCH-R1-UNDECLARED-CONSUMER` and six
  `ARCH-R4-SEALED-MODULE-UNBASELINED-LINK`. The entry is load-bearing, not
  decorative.
* Add one site to the permanent entry that nothing matches (`backup-tests`) and
  the check **fails with `ARCH-R4-STALE-EXCEPTION`**. Permanent does not mean
  unpoliced: a permanent entry cannot rot any more than a temporary one, and a
  seventh impl-subject linker still has to be argued for.

**One consequence recorded rather than hidden.** The seal on `clash_mihomo_impl`
now never lifts, because one of its covering exceptions is permanent.
`arch_check.py`'s `check_seals()` docstring still says both clauses "lift
automatically when the last named exception is deleted" — true of the mechanism,
untrue of this module's outcome. `arch_check.py` is a test source and was not
edited; the divergence is written into `_sealed_modules_note` in
`architecture.json`, where the reader of the ledger will meet it.


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
consumer.) Nothing in `src/**` links it — `clash-qt` does not. It is a
well-tested foundation with nothing built on it yet, and the loader is the thing
that would change that. That is not a criticism of COMPONENT-BASE, which was
sequenced deliberately ahead of its consumers — but it should be visible in the
ledger rather than discovered by someone grepping the link graph in P4.

**Re-verified 2026-09-22 by P3-RECORDS** from the evaluated graph in
`build-records`: `clashqt_com -> ['arch_probe_clashqt_com',
'component-contract-tests']`, unchanged. It is instance 5 of the
producer-removed/consumer-left class recorded below, in its consumer-missing
direction, and it is the one instance with a named owner and a phase.

**A second target carries the same absence and did not have a record.**
`make module` (`Makefile:75-76`) builds `clash_mihomo_impl`, `clash_platform` and
the Go core. It never touches `clashqt_com` — the component the target is named
for. The recipe's own comment says so plainly ("The separately packaged component
does not exist yet… Today this builds the reusable backend libraries and the
engine"), so this is not concealment; but a comment inside a recipe is not read by
the person typing the target's name. Either the target is renamed until
COMPONENT-ABI lands, or it builds `clashqt_com` as well. Owner: holder of the
`Makefile`.


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

**It recurred once, and is now closed.** R5-DOCS found the same failure mode
while reconciling `tests/README.md`, in the one workflow suite that builds
widgets. `w03-routing-controls` was named in **both** lists at the end of
`tests/CMakeLists.txt` and received **neither** `QT_QPA_PLATFORM=offscreen` nor
`QT_QUICK_BACKEND=software`. As measured then:

```
tray                 => [CLASH_QT_DATA_DIR=..., QT_QPA_PLATFORM=offscreen]
home-page            => [CLASH_QT_DATA_DIR=..., QT_QPA_PLATFORM=offscreen, QT_QUICK_BACKEND=software]
w03-routing-controls => [CLASH_QT_FAKE_CORE=..., CLASH_QT_DATA_DIR=...]
```

The cause was different from the first occurrence but the shape identical: the
test is registered in the `tests/workflows/` subdirectory, so
`if(TEST w03-routing-controls)` was false in the parent scope where the block
runs, and CMake emits no warning at configure time. Moving the block to the end
of the file fixed ordering; it did not fix scope.

**Fixed in `ac60175`**, in the subdirectory — exactly where R5-DOCS said it
belonged, beside the `clash_qt_isolate_test()` mechanism the other subdirectory
suites already use. `tests/workflows/CMakeLists.txt` now sets both variables in
the same `set_tests_properties(w03-routing-controls …)` call that sets
`RUN_SERIAL` and `CLASH_QT_FAKE_CORE`, with a comment naming this record and
`447c381`. The parent block carries the other half of the lesson: "Only tests
defined in THIS directory can be listed: `if(TEST ...)` is false here for a test
registered in a subdirectory, so naming one would set nothing and say nothing."

**Verified by P3-RECORDS**, 2026-09-22, from `ctest --show-only=json-v1` in
`build-records`:

```
w03-routing-controls => [CLASH_QT_FAKE_CORE=..., QT_QPA_PLATFORM=offscreen,
                         QT_QUICK_BACKEND=software, CLASH_QT_DATA_DIR=...]
```

**The part that generalises.** Both occurrences were `if(TEST …)` returning false
and CMake saying nothing — once because the test did not exist *yet*, once
because it did not exist *here*. A predicate that is false for two unrelated
reasons, with no diagnostic for either, is not a guard. The durable fix is that a
suite's environment is set where the suite is registered; the lists at the end of
`tests/CMakeLists.txt` survive only for the suites that directory owns.


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


## Recorded defect class — one half of a pair was removed, the other half stayed

**Recorded 2026-09-22 by P3-RECORDS.** This is not an incident record; the
incidents are above. It is the *class* they belong to, written down so it is
hunted deliberately rather than stumbled on. It has produced **six** findings in
this project so far — more than any other single cause — and every one of them
was invisible to a green suite.

**The shape.** Two things were wired to each other. One end was replaced, moved
or deleted; the other end was left exactly where it was. The survivor still
compiles, still links, still passes the tests that were written for it, and is
connected to nothing. Nothing in the toolchain objects: an unused private member
is legal, an emitted signal with no receiver is legal, a link edge no source
needs is legal, and a `make` target that builds the wrong thing exits 0.

**It runs in both directions.** The producer can go and leave the consumer
(`serviceRunning_` has readers and no writer), or the consumer can go and leave
the producer (`systemProxyStateChanged` has an emitter and no receiver). Looking
for only one direction finds half of them.

**The six, with the evidence to re-check each:**

| # | Finding | Which half went | Detected by | State |
| --- | --- | --- | --- | --- |
| 1 | **Privileged-service injection.** Discharging D2 replaced `CoreProcess`'s `platform::PrivilegedServiceClient` with `NullPrivilegedCoreService`; `main.cpp` was never taught to inject the real one, so `setUseService(true)` failed at every call and the saved preference was discarded silently. `PrivilegedServiceClientAdapter` existed, was linked, and nothing constructed it | producer (the injection site) | an audit reading `main.cpp`; 38/38 passed throughout | **fixed** `d697d45`; pinned by two `core-process` cases and by `app-smoke`'s `theCompositionRootSelectsServiceModeWhenTheUserSavedIt` |
| 2 | **Cold-start system-proxy deadlock.** The checkbox was the only thing that ever set a proxy target; the controller reported the proxy unavailable until a target existed; `renderSystemProxy()` disabled the checkbox while unavailable. A fresh install could not turn the system proxy on from any surface | producer (the only target-setter, behind a guard that needed it) | the journey lane — `w03-routing-controls`, written as an *expected* failure that became an unexpected pass | **fixed** `e592003`; `SettingsPage::publishProxyTarget()` now runs whenever endpoint, ports or bypass move (`src/ui/pages/settings/settings_page.cpp:215-227`), and the case is kept as a named regression |
| 3 | **`ServiceSettings::serviceRunning_`.** At HEAD `0835c2a`: declared at `src/ui/pages/settings/service_settings.h:49`, read at `service_settings.cpp:130`, `:167` and `:191`, **written nowhere**. Its only writer was the second `PrivilegedServiceClient` that D3 removed in `88ef6cd`. Permanently `false`, so the guard that should refuse to install, repair or remove the helper **while a core is running under it** was inert — the helper was uninstallable-while-in-use in the one direction that matters, and the D3 commit's own note recorded the flag as left in place "so re-wiring it is one line" | producer (the second client) | reading the page after D3; no test asserted an uninstall guard | **fixed during this wave** by a parallel worker, through the contract rather than a second socket — see below |
| 4 | **`SettingsPage::systemProxyStateChanged` / `systemProxyBusyChanged`.** Both emitted at `src/ui/pages/settings/settings_page.cpp:277-278`, declared at `settings_page.h:47` and `:51`, with **zero `connect()` calls anywhere in `src/` or `tests/`** | consumer. `src/ui/shell/main_window.cpp:233` connected `systemProxyStateChanged` until `88ef6cd` deleted the connection — the shell now reads `RoutingController` instead. `systemProxyBusyChanged` is worse: it has had no receiver since `2412852` first added it | `grep -rn` for the signal names | **open, unowned.** Either delete both, or give them a receiver. Deleting them is the likely answer, but it is a UI decision and P3-RECORDS holds no `src/` file |
| 5 | **`clashqt_com` is linked by nothing that ships.** `src/core/CMakeLists.txt:34` builds it; in the whole evaluated graph its only consumers are `component-contract-tests` and the checker's own `arch_probe_clashqt_com`. `clash-qt` does not link it | consumer — the loader that would have used it was never written | the evaluated link graph, read by hand | **open, owned.** Recorded in the `src/integrations/component/**` deferral above; P4/COMPONENT-ABI. Not a criticism of COMPONENT-BASE, which was sequenced ahead of its consumers deliberately — but a well-tested foundation with nothing on it looks identical to a finished one |
| 6 | **`make module` does not build the module.** `Makefile:75-76` builds `clash_mihomo_impl`, `clash_platform` and then the Go core. It never touches `clashqt_com`, the component it is named for. The recipe's own comment is honest about it — "The separately packaged component does not exist yet… Today this builds the reusable backend libraries and the engine" — but the target name is what people run | consumer (the packaged component the target was named for) | reading the recipe | **open.** The comment is not the defect; the name is. Either the target is renamed, or it builds `clashqt_com` too. Holder of the `Makefile` |

**Why this class is so productive here, specifically.** A refactor of this shape
— sever a coupling, publish a contract, move the wiring to a composition root —
*is* a machine for producing it. Every discharged edge is one half of a pair
removed on purpose. The other half is by definition somewhere else, in a file the
package that discharged the edge did not hold, and single-writer file ownership
means nobody was looking at both ends at once. Findings 1, 3 and 4 are all direct
consequences of a coupling this refactor removed correctly.

**How to look for it, deliberately.** None of the six was found by a test
failing. Five were found by reading; one (the deadlock) by a journey written
pessimistically. The cheap sweeps, in the order that has paid:

1. **Members read and never written** (and fields written and never read).
   `serviceRunning_` was three reads and no writes. A compiler will not say so
   for a member; a grep for the name, counting assignments, will.
2. **Signals with no `connect()`.** `grep -rn "<signalName>" src tests` and look
   for the absence of `connect`. Do it for every signal a migration touched.
3. **Targets nothing links.** Read the evaluated graph, not `CMakeLists.txt`:
   `arch_check.py` already parses the CMake File API, and a library whose only
   consumers are its own test and `arch_probe_*` is the signature.
4. **Build and run targets whose name is a promise.** `make module`, `make run`
   (which once exported `CLASH_QT_CORE_BINARY` that nothing read — the same class
   again, found the same way) — run the documented command and check it did what
   the name says.
5. **Whenever a coupling is severed, grep the far end in the same sitting.** The
   package that removes a producer does not own the consumer's file, which is
   exactly why the consumer survives.

**What would catch each automatically, and what would not.** Findings 1 and 2 now
have tests and would fail if reintroduced — 1 in `app-smoke` because it drives the
shipped binary, 2 in `w03-routing-controls` because it starts from an empty data
directory. Findings 3, 4, 5 and 6 have **no automated detector at all**. A green
run says nothing about them, and the next instance of this class will be found by
reading unless a rule is written. That asymmetry is the finding worth keeping: the
lanes that catch this class are the ones whose subject is the *assembly* — the
shipped binary, and a journey with nothing pre-seeded.

### Finding 3's end state — which way it went

A parallel worker discharged `serviceRunning_` **during this wave**, and it went
the way the D3 record implied it should: **the flag is re-published on the backend
contract, not by restoring a second privileged connection.**
`core::backend::PrivilegedServiceStatus` (`src/core/backend/capabilities.h`) gains
a `coreRunning` field carrying the helper's own `state == "running"` report —
which is machine-wide, because the macOS helper keeps exactly one core and reports
it to every connection, so it is deliberately *not* this backend's `CoreState`.
`BackendBridge::privilegedServiceStatus` carries it to the UI as a fourth
argument, and `ServiceSettings` writes `serviceRunning_` in exactly one place
(`src/ui/pages/settings/service_settings.cpp:97`), inside the branch that runs
only when `state == ServiceState::Connected` and the error is empty. An
unanswered query leaves the flag alone, because not hearing back is not evidence
that nothing is running — treating a `false` that means "no answer" as "no core"
would disarm the guard a second time, in a way no compiler and no type would
catch. The guard is exposed as `ServiceSettings::canChangeInstallation()` so a
test can assert it without clicking Remove on a machine that has a real helper
installed, and a new suite `tests/ui/service_settings_test.cpp` pins it.

The alternative — restoring the page's own `PrivilegedServiceClient` — would have
re-opened D3 to close a defect that D3 itself caused. It was not taken. One
connection to the privileged socket, and the contract carries what the UI needs.

**Status when P3-RECORDS wrote this: complete in the working tree, uncommitted.**
P3-RECORDS holds no `src/` and no test file, and read the shape from the diff
rather than from a build; the description above is what the change does, not a
verification that it builds. One consequence for this file's own numbers:
`tests/ui/service_settings_test.cpp` exists but `tests/CMakeLists.txt` does not
yet register it, so **`ctest -N` = 47 and the `make test` lane = 44 are correct
only until it is registered** — at which point both move by one and
`tests/README.md`'s registered-suite table needs the new row. Every count in this
ledger is a measurement with a timestamp, not a property of the branch.


## Gate status — measured 2026-09-22 in `build-r5d`

Configured and built fresh at `build-r5d` (Ninja, Debug, `BUILD_TESTING=ON`,
`CMAKE_PREFIX_PATH=/opt/homebrew`) so that no other worker's build tree was
touched. `ctest -N` registers **47** tests.

| Lane | Selection | Registered | Result |
| --- | --- | --- | --- |
| `make test` | `--label-exclude "native\|privileged\|benchmark\|real-core"` | 44 | **44/44 passed** (final run, 2026-09-22, after the E1 discharge landed) |
| `make test-integration` | `--label-regex "real-core\|integration"` | 15 | not run here: it needs `make core`, and `build-r5d` does not stage the engine. The coordinator's figure is 15/15 |
| `make test-native` | `--label-regex "native\|privileged"` | **0** | both labels are reserved and carried by no registered test. The privileged suites carry `service` |

**`make test-native` matches nothing, and that is a reservation rather than a
defect — but it must be said, not implied.** Re-verified by P3-RECORDS in
`build-records`: `ctest -N` = 47, `--label-exclude "native|privileged|benchmark|
real-core"` = 44, `--label-regex "real-core|integration"` = 15, and
`--label-regex "native|privileged"` = **0**. No `clash_qt_label` call anywhere
under `tests/` passes either word. The lane exists — with its
`CLASH_QT_NATIVE_HOST=1` confirmation gate and its exclusion from `make test` —
so that the first genuinely host-exclusive test has somewhere to land. Until one
is written, the target succeeding means **nothing ran**, not that native or
privileged behaviour was verified. Two of `make test`'s four excluded labels are
in the same position of naming a category rather than a body of tests; the
`Makefile` help line "Native and privileged tests. Opt-in; needs a disposable
host." is the sentence most likely to be misread, and `tests/README.md` now says
so at the head of its registered-suite table.

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
| ~~D3's second privileged connection~~ | **Closed in `88ef6cd`.** This row said "Still open" for two commits after the fix landed |
| ~~E1~~ | **Discharged in `ac60175`.** This row said "In flight, not discharged" while the same file, 400 lines earlier, recorded the discharge with its evidence. Two rows of one table disagreeing is the drift this wave was called to fix |
| ~~`w03-routing-controls` unguarded against a headless runner~~ | **Fixed in `ac60175`**, in `tests/workflows/CMakeLists.txt`. Both variables verified present in the configured tree |
| `make test-native` | Runs **zero** tests and always has: `native` and `privileged` are reserved labels carried by no registered suite. Not an open gate — a reserved lane. Recorded here so the row is never read as coverage that regressed |


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


## P4 resumed — 2026-09-22, base `eb7bab1`

Coordinator resumed from this ledger, D1–D8 and git log. Initial working tree
clean on `codex/refactor-v2`. No changes to main, legacy-v1 or remotes.
Worker transport: installed Claude CLI, each dispatch explicitly `--model opus`,
no fallback and no child agents. Availability probe resolved to `claude-opus-5`.
Maximum three concurrent workers. Initial ABI, CFG-CORE and CFG-UI/W02 analyses
are read-only; implementation leases are recorded in `P4_DISPATCH.md`.
Coordinator retains shared architecture, contracts, concurrency, git, main.cpp,
all build files and architecture.json. No builds during source-edit leases.

Scratch evidence: `/tmp/clash-qt-p4.w0Y8Uo` (ephemeral; durable findings recorded
here before completion). Initial native preferences SHA-256:
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`.
No tests/apps have run yet. Tests require isolated CLASH_QT_DATA_DIR and
before/after native plist hashes. Installed helper/network/trust remain untouched.
P4 gates remain OPEN; Windows/Linux evidence remains zero (G5).

### P4 configuration and UI implementation barrier

CFG-CORE and CFG-UI workers both returned STABLE (`claude-opus-5` explicitly
selected; no fallback/nested agents). Source remains uncommitted pending combined
integration; no full-tree build claimed. Their external snapshots exercised the
real composer/ProfileStore: eight config suites and the preset-editor suite pass.
Twelve config and eleven UI inversions detected; durable reports/preferences logs
are in `.refactor/evidence/p4/`. Setup/teardown are not product cases.

The config worker's final report incorrectly called main.cpp's presetsChanged
connection absent and four test registrations outstanding: these had already
been written by the coordinator. Verified from actual source instead of accepting
that report. The missing fifth suite, chain-snapshot, is now registered.

A producer/consumer sweep found the new preset documents absent from BackupStore's
explicit roots/allowedPath lists. An Opus worker now owns that bounded integration
and its backup tests. Another Opus worker owns W02 and migration of all existing
journeys to ModuleLoader/ModuleBackend. The initial ABI worker still owns its
source; no builds over any active lease. Initial ABI boundary suite is green in
its own external snapshot, but common-backend parity, retained-object lifetime,
independent sample, installed bundle and final read-only audit remain OPEN.

Native preferences SHA-256 before and after all completed CFG/UI runs is
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`;
both values per run are retained in the evidence logs. Windows/Linux: zero evidence.

### P4 ABI correction — green boundary suite did not prove unload

The independent sample drove a source-built engine to ready and confirmed stop,
but actual dlclose after activation segfaulted (139). Omitting dlclose avoided it.
The first ABI worker inferred Qt registry retention without a crash stack and
introduced PinnedOnceActivated/kFalse. Coordinator rejected that as closure of
the explicit unload gate; a fake/inactive module unloading is not production
unload evidence. `P4_ABI_REVIEW.md` records the required targeted fixes and expanded
private-source lease. The follow-up worker must diagnose the actual retained
callback and demonstrate real unmapping, not restate the workaround as success.

The initial ABI suite/benchmark/inversions remain useful boundary evidence but
not P4 acceptance. Initial report is preserved under evidence/p4/abi-initial-report.md
with this correction taking precedence. It also called clashqt_com visibility
unfixed after the coordinator had set it; clash_yaml's missing hidden visibility
was real and is now set. Treat every worker handoff as a claim to check.

External documentation commit `17e65d1` appeared while workers ran: NAMING_PLAN.md
plus some dispatch notes. Preserved as found; no implementation or main ref change.

### P4 safety incident — the smoke guard covered start, not status

Coordinator traced ServiceSettings construction to its unconditional queued
checkStatus(), then requestPrivilegedServiceStatus → requestServiceStatus →
PrivilegedServiceClient::requestStatus. main.cpp injected the default installed
helper socket. The old app-smoke >8 MiB guard prevents startCore, not this status
connection. Earlier P4 snapshot app-smoke runs used that path; we cannot claim
those runs avoided the installed helper. No install/uninstall/restart was invoked.
Do not probe/connect the installed socket to reconstruct history.

Stopped W02 worker PID 59482 with SIGINT and verified it exited before assigning
further app runs. No app child was listed in the scoped process check. Added
CLASH_QT_SERVICE_SOCKET at the composition root, feeding both the client and
adapter (absolute explicit endpoint, default unchanged). CTest supplies an isolated
socket. W02 follow-up must make child environments always choose their own socket,
prove startup status goes to that local fixture, and re-run smoke on the fixed
executable before any further app-launch evidence is accepted. --data-dir by
itself is not sufficient isolation for machine-wide helper IPC.

Defense at the composition root: when CLASH_QT_DATA_DIR is selected (including
--data-dir), an omitted service-socket override resolves to helper.socket in that
isolated directory, never the installed default. Normal launches with neither
setting retain the normal endpoint. Mutation proofs for this guard must substitute
a DIFFERENT ISOLATED socket; never mutate a test launch back to the real helper.

### Stable config/UI/backup packet committed

`e5d7d7a` records the stable configuration extraction, versioned presets,
side-effect-free preview, UI controls and backup compatibility plus adjacent
tests. Explicit pathspec commit; no active ABI/core lease files swept in.
Central whole-wave wiring remains uncommitted, so this intermediate commit is
not a standalone P4 qualification point. Independent config subgate audit is
running against these stable sources. ABI, shared contract parity and fresh-clone
package/audit gates remain OPEN. main and legacy-v1 have not been changed.

### Stable ABI packet committed; independent subgate checks active

`2a0c577` records the module/loader/shim, fake module, independent consumer, ABI
qualification tests and marshalling benchmark. Final module wire revision is 2;
backend-r4/Qt bridge signatures remain unchanged. The debugger proved the original
unload crash was a retained QtNetwork cleanup callback after that *runtime library*
was unmapped. The component now actually unmaps while its shared runtime remains
resident for host cleanup; real-engine consumer exit is 0. Independent ABI audit
still checks root-reference retry, cross-thread final Release and date-time fidelity.

Core parity correction still owns MihomoClient/common-test files; no combined build
has run over that lease. Config subgate auditor owns no repository source.
Benchmark qualification now records timings without assuming packed < naive in every
build: the initial speed assertion failed in Debug despite unchanged codec. D8 asks
for measurement; correctness/bounds remain hard assertions in the routine lane.
No Windows or Linux C++ compile/run evidence exists, regardless of conditional code
or any worker wording saying those paths 'compile'.

### Independent config subgate audit found real defects

Auditor was read-only, used its own snapshot and 17 differential corpus rows plus
12 detected inversions. It returned a narrow CONFIG GO but reported F1 high:
unreadable presets.json silently discarded presets and bypassed last-good; next
save made the loss permanent. F2 medium: merge depth limit silently truncated data
while reporting success. Coordinator does NOT accept those as non-blocking. A
bounded config-fix lease addresses both and F3 (direct pure-compose mixed-port
override). Original audit evidence retained under evidence/p4/config-audit-initial.md.
Cosmetic mapping-key order and unmeasured main-thread read cost are not fabricated
as failures or performance evidence. Full P4 gate remains open.

### Independent ABI subgate audit — NO-GO

Own stable snapshot, independent probes, Debug and Release; 53 isolated runs with
before/after native plist hashes unchanged. Proved activated production image
unmapping, buffer/error/nested-buffer retention, handshake canaries, production
observer admission and no private helper/proxy-data boundary. But it reproduced:

1. Activated final Release from another thread destroys QProcess/socket notifiers
   there and crashes (SIGSEGV). Session was counted dead while cleanup survived.
2. Holding a root reference makes first unload refuse correctly, then release/retry
   cannot unload: loader detached its root and module latched unusable state.
3. Epoch-only QDateTime encoding changes UTC/fixed-offset/named-zone presentation
   despite operator== passing, observable in connection details.
4. Decode failure diagnostic could be absent/stale.

Coordinator assigned a bounded final repair, not a waiver. Detailed independent
finding record: evidence/p4/abi-audit-no-go.md. Native prefs remain
681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c.
The coordinator's four backend integration CTest entries passed in the real build
wiring (stable source subset only); exported module symbol list was exactly the
C factory. Those greens do not override the audit's NO-GO.

### Isolated startup must not inspect another client by default

A separate producer/consumer trace found that app-smoke removed the controller
variable for generic launches, while discovery could read another installed
client's controller. A bounded isolation lease makes isolated/explicit-invalid
controller selection authoritative (no implicit foreign/default attachment), and
keeps traffic subscription intent for a later managed endpoint. The coordinator
also disables automatic legacy geo-data seeding for isolated data directories;
fixtures provide their own seed directory when needed. No claim is made that the
previous generic smoke launches avoided external-controller discovery. New app
qualification will run against these isolation guards.

### Config corrections independently rechecked — GO

`7b47a02` closes F1/F2/F3. A new read-only Opus reader used its own byte-identical
snapshot and fifteen suites, including independent recovery/composition/journey/UI
probes. Fifteen targeted flips were all detected and restored. It verified valid
last-good recovery for unreadable/missing/directory/dangling primary, preserved
memory state, appending edits retaining recovered content, diagnosed over-depth
merge refusal without runtime writes, valid trusted port override/provenance,
preview no writes/latest-wins, real-store UI, and backup compatibility.

GO is for configuration, not all P4. Cosmetic YAML key order and owning-thread
snapshot I/O cost are not claimed as improved. ABI NO-GO findings remain assigned.
Current native preferences before/after all recorded recheck runs are identical.

### Actual central wiring checks (not final qualification)

Stable subsets built in `/tmp/clash-qt-p4.w0Y8Uo/coordinator-configure`, never
build/dev: backend-real-contract, controller, ABI and snapshot codec **4/4**;
config/composer/preset/document/preview/UI/backup/legacy generation **10/10**.
All executed via paired-hash isolated runner; native plist before=after equals the
original full SHA. Logs retained under evidence/p4/. No full source build crossed
an active lease. These are not a substitute for the pending fresh-clone gate.

### Targeted transport audit — another NO-GO, tested

Read-only HEAD archive at f04647f, local fixture only. It proved provider requests
coalesce across a replaced session (same retired RequestId, no new request), old
provider and REST completions can remain Ok across lifecycle generation bumps,
and old stream frames/drop events are stamped into the replacement generation.
The fake's identical-endpoint early return hid the same boundary. Detailed evidence
is evidence/p4/transport-audit-no-go.md. A bounded transport lease now addresses
one coherent invalidation path across all three transports and the four shared
contract implementations. No waiver or expected-failure marker closes this gate.

The ABI wire-3 repair fixed the three independent ABI findings and decode diagnostics,
but its deferred Release still returned zero before object destruction. A final
small RC lease preserves component-r1's existing zero-return promise by keeping a
cleanup reference or separately counted native state, without changing the facade.

### Release return promise retained

RC worker returned STABLE: deferred owner/native-work cleanup holds a real
reference and returns nonzero; synchronous destruction returns zero afterwards.
Forty-five ABI cases pass in Debug and Release, and early-zero/premature-count/
ignored-native-work/Close-consuming-caller-reference inversions fail. The prior
cross-thread probes' expectation `Release()==0` is intentionally corrected; their
module count/refusal/drain/unmap assertions remain. Independent reader received
RC_STABLE and will verify the resulting contract, not accept the worker report.
A deliberately crashing scratch inversion left one owned engine; the worker
terminated that exact process and confirmed its port free. No installed helper use.

### Independent final ABI recheck — GO for the ABI subgate

Read-only reader used a frozen snapshot, synchronized only the finished RC files,
and tested Debug and Release. Its new probes independently covered timestamp
representation through the actual boundary, retained root/interface retry with
RTLD_NOLOAD checks, combined buffer/deferred retention, zero/nonzero Release meaning,
caller-reference preservation, native work and actual activated unmapping. Forty-five
ABI and sixteen snapshot cases pass. Fifteen of sixteen targeted mutations detected;
the remaining single validator was redundant and removing both validators failed.
Details: evidence/p4/abi-recheck-go.md. Destructor-ordering inversion remains an
explicit unverified coverage detail, not fabricated evidence. Windows/Linux have
zero compile/run evidence. Dead stale fake-protocol size constant was removed
(coordinator; actual consumer uses marshal::kMinConnectionBytes).
Whole-P4 still awaits transport/deadline fixes, fresh-clone gates and final audit.

### Transport generation repair stable

Transport repair returned STABLE: one session boundary orders epochs → owner bump
→ participants/provider retirement → TUN cancellation → abort. Same-address attach
replaces streams; lifecycle invalidation retires requests without recursion or
unnecessary stream churn. Fake replacement now obeys the same rule. Shared suite
now includes 17 cases × 4 implementations plus specialist/stream cases (105 reported
passes); controller 23, provider 6. Seven targeted inversions detected; two isolated
defensive checks survived as recorded redundancies, not independent coverage claims.
A snapshot-wide 57/57 later passed but W05's earlier measured coarse-timer failure
remains assigned to timer-fix and is not excused by that later green.

### Final implementation source barrier

All source writers returned STABLE. The W05 defect was reproduced independently:
Qt's default singleShot switches to CoarseTimer at >=2 s; 3000 ms fired as early
as 2851 ms, while the old 400 ms test selected precise timing and hid it. The fix
uses a precise monotonic deadline and rechecks before kill. Published timings are
unchanged; test lower bounds are not weakened. Refusal marker is now an explicit
fixture precondition. Shortened-budget and restored-coarse-timer inversions fail.
Detailed evidence: evidence/p4/termination-deadline-report.md.

Coordinator now integrates the complete source/build/test/packaging wiring, then
qualifies a fresh git clone using BUILD_DIR=build/p4. No build/dev writes. Final
make test, integration, package, independent SDK consumer, packaged launch,
marshalling measurements and whole-P4 independent audit remain to be recorded.

### Fresh-clone facade finding

Candidate ac3d8e6 cloned cleanly with the recorded mihomo gitlink; `make test
BUILD_DIR=build/p4` built and passed 54/54. Qualification wrapper then found no
LastTest.log under build/p4: CTest --preset dev retained its log directory at the
*disposable clone's* build/dev despite --test-dir selecting build/p4 tests. No
compilation used build/dev and the original working-tree build/dev was untouched.
The facade now uses --test-dir with explicit output/no-tests options, equivalent
to the presets without their fixed binary directory. A new clean clone will
rerun all three Make gates; the first partial result is retained, not overwritten.

### Coverage index and explicit checker isolation

The configured registry contains 59 entries: routine 54, integration 20, native/
privileged 0. tests/README.md is reconciled to that actual registration, including
shared/backend/module lanes and W02 split functions. Architecture checks already
inherited the qualification runner's isolated data directory; they now also get
explicit per-entry CLASH_QT_DATA_DIR for direct CTest use, just like every other
entry. This final registration change will be included in the final clean clone.

### Headless registration correction

Release benchmark setup with CLASH_QT_BUILD_APP=OFF exposed unguarded W03/app-smoke
registrations: Qt6::Widgets and the clash-qt executable were absent but referenced.
Coordinator gated those two desktop-only entries at their registration site; four
headless journeys and W02-real-core remain available. This is a build registration
fix, not a re-baselined dependency. The final clean clone includes it.

### Fresh W02 readiness race closed

W02's old Running/process-count checks observed the outgoing core while validation
was pending; the worker reproduced the original 2-vs-1 failure under load. The test
now records coreReady completion generation and requires newer readiness, no pending
restart and exactly the refreshed active config before process/applied-config checks.
Fixed real-core runs: 5 idle and 8 loaded; fixture 3 idle and 2 loaded. Old-position
and wrong-retention inversions fail; other redundant predicate inversions are not
claimed as proven. Only W02 test source changed. Details in
 evidence/p4/w02-readiness-report.md.

Package preflight on frozen fresh-2 staged successfully. Native Cocoa launch with
explicit isolated data/helper, no module override or developer dependency paths,
loaded the bundle-relative module and stayed alive; no Homebrew/Anaconda images.
The offscreen probe initially failed because the production bundle ships Cocoa,
not offscreen; no native-GPU correctness claim is inferred. Installed-SDK consumer
built separately (QtCore + system libraries only), drove matching pinned engine,
confirmed stop and independently reported actual image unmapping. Full final Make
qualification will run on a new clone including W02 and headless registration fixes.

### Package exit-zero was not a clean package gate

Fresh-3 at edb78ec passed routine54/54 and integration20/20 without required
skips/XFAIL, and make package returned0. Its Qt deployment log nevertheless reported
an unsigned libclash_qt_backend_module.dylib nested code object. Coordinator does
NOT treat that as clean qualification. The module is now explicitly passed to Qt
deployment as an additional executable/module for both development and install
bundles. After the pinned engine/manifest are copied, the completed developer bundle
is ad-hoc signed (no certificate/trust-store change) and verified deep/strict with
failure propagated. The engine itself is not re-signed, preserving its build hash.
This packaging fix needs a new clean qualification run; prior logs are retained.

### Explicit nested signing required

Fresh-4 routine54/integration20 passed, but package failed as intended after the
new verification: Qt deployment's fake signature remained on the nested module;
post-install bundle signing alone did not replace that nested code signature.
The package script now explicitly ad-hoc signs the module dylib and helper first,
then the bundle, then verifies deep/strict. The package gate remains OPEN until a
new clone's install completes without deployment ERROR lines and deep verification
passes. No certificate, trust store or engine re-signing is used.

### Controlled developer signing sequence verified

Explicit Qt additional-module registration still left Qt's automatic signing with
an unsigned nested dylib. A disposable artifact probe deep-signed the deployed
bundle *without* its engine, restored the exact built engine, signed the outer
bundle without --deep, and passed deep/strict verification with engine SHA unchanged
(6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544).
Packaging now runs Qt deployment with -no-codesign, signs the nested bundle before
installing the engine, then seals/verifies the completed app. Repeat staging removes
only the prior generated engine copy before deployment. No signing credentials,
network timestamp or trust-store operation is used. New clean qualification pending.
