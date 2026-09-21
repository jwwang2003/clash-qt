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
| PRE-ARCH | worker A / Opus | — | scratchpad only | running | — |
| PRE-TEST | worker B / Opus | — | scratchpad only | running | — |
| PRE-MOVE | worker C / Opus | — | scratchpad only | running | — |

## Next ready packages

P1 relocation (MOVE-UI, MOVE-CORE, MOVE-TEST) once P0 findings are consolidated and
ownership is published. Relocation is a mandatory whole-tree barrier: no dependent
package starts against the intermediate tree.
