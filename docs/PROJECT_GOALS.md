# Refactor project goals

Status: goals and acceptance criteria, 2026-09-21. These are implementation targets,
not completed features. They take precedence over optional scope in earlier plans.
No branches, source files, build scripts or installed packages are changed here.

## G1 — Build the managed mihomo from local source

The managed engine is built from `3rdparty/mihomo` at the superproject's recorded
submodule commit. The reviewed checkout is `ab405bad5beeeac8b003bb01f60f134f6df54471`;
this is provenance for the current plan, not a requirement to freeze that version
forever. Updating it is an explicit reviewed dependency change.

- Build scripts compile the local source and stage its binary beside the component.
- Application packaging and real-core tests consume this exact build output. They
  never silently use PATH, another Clash installation or an upstream binary download.
- Record source commit, dirty state, Go version, build tags, target architecture,
  CPU baseline and artifact checksum. Release builds reject an unexpected dirty
  submodule; developer builds may use local edits with explicit provenance.
- Pin the tested Go toolchain and module dependencies. The local `go.mod` declares
  Go 1.20, but actual minimum/toolchain compatibility must be established by building
  its current dependencies; do not infer the full toolchain requirement from that
  line alone.
- Missing source/toolchains fail with an actionable setup message. Dependency
  initialization is explicit; ordinary builds do not advance the submodule branch.

Acceptance: from a documented checkout/setup, one command builds the local engine,
component and app; packaged runtime and integration tests report matching provenance.
External-controller attachment can remain an explicit feature, separately labeled.

## G2 — Make mihomo a separate component connected through COM

clash-qt application/UI code consumes published component interfaces. It does not
compile MihomoClient/CoreProcess into the UI or reach around the component to issue
private controller requests. The component owns its locally built engine process
and controller implementation; embedding Go into the GUI is not required.

The user's selected reference, `3rdparty/ref/fxcom`, establishes the direction:
**one portable COM-style object/component model on macOS, Windows and Linux.**
Use its interface-query and reference-ownership concepts, with project-owned names,
IDs and audited semantics. Do not retain FX/fx prefixes or unrelated project coupling.
This resolves the earlier terminology assumption and removes the requirement for
native Microsoft COM activation on Windows. See the [COM plan](COM_MODULE_PLAN.md).

- Portable module has an explicit factory/interface boundary and is separately
  built/packageable as a shared library on each OS. Keep Qt/STL implementation types
  behind any published binary boundary. Native Windows COM is a separate optional facade.
- Define interface IDs, ownership, calling conventions, string/buffer layout,
  allocation/free rules, errors, cancellation and version negotiation per supported
  target ABI. Claim only tested compatibility; COM-style is not automatic Microsoft
  COM or arbitrary cross-compiler binary compatibility.
- A separate sample consumer starts/queries/stops the engine without linking the
  desktop implementation. Fake and production modules satisfy common contracts.
- All three distributions use the portable component boundary; linking the private
  implementation directly into the UI cannot substitute for module qualification.
- Module packaging/version checks, unloading after outstanding work drains, and
  process ownership are tested. Privileged execution remains a separate service.

Acceptance: module builds independently; app uses only its approved interfaces;
each claimed platform passes sample-consumer, lifecycle and real-core tests. COM
is a delivery requirement; native Microsoft COM interoperability is not.

## G3 — Provide one documented Make command surface

Provide a root GNU Make facade over CMake presets, Go and CTest. Keep OS-specific
logic in small scripts/adapters rather than duplicating the build graph in Make.
Document required shells/tools on Windows instead of assuming POSIX tools exist.

Planned commands, not available targets yet:

| Command | Contract |
| --- | --- |
| `make help` | List commands, variables and prerequisites |
| `make doctor` | Read-only prerequisite/platform diagnostics |
| `make setup` | Initialize recorded submodules and provision/download declared dependencies explicitly |
| `make configure PRESET=...` | Configure the selected supported native toolchain |
| `make core PRESET=...` | Build local mihomo only |
| `make module PRESET=...` | Build component and its required engine artifacts |
| `make build PRESET=...` | Build the app and all runtime dependencies |
| `make run PRESET=...` | Build if necessary and launch the app with the staged component/core |
| `make test PRESET=...` | Build and run portable feature/architecture/UI tests |
| `make test-integration PRESET=...` | Build local engine and run real component/core workflows |
| `make test-native PRESET=...` | Explicit native/disposable-environment tests, with eligibility checks |
| `make package PRESET=...` | Build and stage a platform-specific distribution |
| `make capture-addons PRESET=...` | Build/package the selected optional monitor and source-based mitmproxy add-ons |
| `make test-capture PRESET=...` | Run capture contracts/workflows with synthetic apps and local endpoints |
| `make clean PRESET=...` | Remove only selected generated output; preserve source, user data and submodule checkout |

Setup may need network access; ordinary builds may use provisioned Go/Qt/dependency
caches. Offline builds require those prerequisites already present. The commands
must propagate errors, handle spaces in paths, and avoid modifying user networking
or installing a privileged helper as a build/test side effect.

Acceptance: fresh-checkout instructions are exercised on each supported OS; CI uses
the same documented commands; build, launch, tests and packaging have no hidden
manual steps. See [build and release planning](BUILD_RELEASE_PLAN.md).

## G4 — Preserve legacy-v1 and replace the old docs after refactoring

Before the first implementation move, preserve a reviewed snapshot of the current
pre-refactor code **and the entire existing `docs/` contents** on `legacy-v1`.
This includes relevant untracked planning documents and the recorded mihomo source
reference. A branch at current HEAD alone is insufficient while those files are
uncommitted. Exclude generated outputs and unrelated/private files from the snapshot.

Perform the refactor on `codex/refactor-v2` based on that preserved snapshot.
Once the refactor/module/build/test/platform acceptance gates pass:

1. Verify `legacy-v1` contains the complete old source/docs and its referenced
   submodule commit can be retrieved. Record the snapshot commit.
2. On the refactor branch, replace all pre-refactor files in `docs/` with a small
   newly written documentation set matching the actual implementation. Do not leave
   obsolete audit/plan documents in `main` under a different archive subfolder.
3. Refresh root README and links, run documentation/link/command checks, then merge
   the refactor into `main` normally. Preserve history; no force-reset or branch
   deletion is needed. Handle concurrent main changes before final qualification.
4. If the repository is shared remotely, preserve the archival branch there as part
   of the explicit migration/publishing step before removing the current docs from
   the released tree. A local-only branch is not a remote archival guarantee.

`legacy-v1` stays frozen as the pre-refactor reference. Cleanup occurs after the
foundation refactor is complete; later business/VPN features can continue on the new
main. Documentation cleanup is a reviewed explicit file-list change, never an early
blanket directory deletion. Do not execute this migration during goal-setting.

Acceptance: `main` contains the validated refactored system and only current docs;
`legacy-v1` retains the old code/docs. Both branch identities and source revisions
are documented and independently retrievable in the intended repository location.

## G5 — Build, test and package macOS, Windows and Linux

Use native CI jobs for each desktop OS. Building a Go target on another host does
not certify its Qt app, COM binding, installer, helper or native networking.

Initial release target matrix: macOS arm64 and x86_64, Windows x86_64, Linux x86_64.
Windows/Linux arm64 are follow-up targets unless explicitly promoted with runners
and dependency evidence. Pin a supported Qt/C++/Go toolchain and minimum OS baseline
per target; do not claim support where only compilation has been checked.

Package the locally built core, its component, Qt/QML/plugins and required runtime
libraries. macOS gets an app bundle/DMG; Windows a portable ZIP then installer with
the portable component DLL; Linux an AppImage or relocatable tarball first, with DEB/RPM added
when their distro support policy is defined. See [the platform plan](BUILD_RELEASE_PLAN.md)
for signing, installation and native feature gates.

Acceptance: clean-machine install/launch, normal proxy traffic, component loading,
upgrade/uninstall and resource resolution pass on each claimed target. Signed
public releases have signing/notarization gates; unsigned developer artifacts are
clearly distinct. Missing credentials/runners stay visible blockers to that claim.

## G6 — Test the architecture and product workflows

Implement [the test strategy](TEST_STRATEGY.md): dependency boundaries and component
replacement; ordinary feature contracts F01–F10; complete workflows W01–W05; local
real-core integration; installed-package/native checks. Preserve useful regressions
without making past bug reports the sole source of test cases.

Acceptance: every key feature has success/failure/recovery evidence, tests link the
intended production component, native limitations are explicit, and CI runs the same
Make commands documented for developers. A module/library compiling is not enough.

## G7 — Containerized application network monitor

Provide an optional monitor module that selects a host app or container workload,
reports incoming/outgoing connections, endpoints, destinations and bytes, and runs
its analysis/storage service in an owned container. Host-app attribution comes from
a qualified native sensor, not an assumption that a container sees the host network.
Show URLs only when observed, such as plaintext HTTP; HTTPS paths remain unavailable
without interception. Report hostname provenance and unknown/partial coverage.

Acceptance: selected and unrelated fixture apps are distinguished, qualified inbound
and outbound traffic is captured with correct direction, passive mode retains no
bodies/CA changes, and container/sensor failures report gaps and clean up owned
resources. See [CAPTURE_MODULE_PLAN.md](CAPTURE_MODULE_PLAN.md).

## G8 — App-targeted MITM capture, export and scheduled jobs

Provide a separate module based on local `3rdparty/mitmproxy`, with target selection,
decoded request/response inspection, native capture files, bounded exports and
persisted periodic capture jobs/scripts. Intercept qualified protocols with target
trust configured; preserve application payload semantics by default while forwarding
and re-encrypting both directions. Do not promise every app/protocol or identical
wire bytes. Pinning, custom trust, mTLS, application encryption and inbound-server
targeting have explicit capability/compatibility states.

Acceptance: local HTTP/HTTPS fixtures retain request/reply content, selected apps
are isolated, saved flows reopen/export, full-body limits are explicit, and scheduled
jobs handle process restarts, overlap, sleep/DST, scripts, cancellation and disk errors.
No certificate installation or capture begins just because an add-on is installed.
The detailed monitor/MITM scope and source-backed limits are in the capture plan.

## Ownership, scheduling and release scope

The coordinator owns the goal/contract ledger, Make/presets/shared build wiring,
archival snapshot and final branch transition. Parallel workers implement bounded
source-build, component, test and platform packages under the existing ownership
rules. G1/G3 begin in P2, G2 foundations begin in P2/P3 and complete with portable
module qualification on each native OS,
G5 planning starts in P0 and acceptance runs per-platform, G6 spans every wave.
G4 snapshots in P0 and cuts over after all foundation gates are met, not necessarily
after optional business/VPN/plugin product tracks P5–P7 finish.

G7/G8 are requested add-on scope delivered in capture workstreams P8/P9 (or earlier
freed slots after their real prerequisites land). They share targets/catalog/schedules
and the same portable COM foundation, but have different visibility/trust contracts.
They do not block the G1–G6 foundation/main cutover.

The foundation release requires G1–G6 for its claimed target matrix. Future OAuth,
management, Tailscale and EasyConnect scope remains in the architecture roadmap.
These goals do not require waiting for every future feature before the refactor can
become the new main; they do require the local core/module/build/test/platform work.
