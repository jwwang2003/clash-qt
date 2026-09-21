# Build, package and migration plan

Status: proposed, 2026-09-21. Implements goals G1/G3/G4/G5 in
[PROJECT_GOALS.md](PROJECT_GOALS.md); commands/files below are planned deliverables.

## Source and build pipeline

```text
Recorded 3rdparty/mihomo commit + provisioned Go toolchain/modules
  -> target-specific local mihomo executable
  -> separately built portable COM/component implementation (reference-inspired, no FX naming)
  -> clash-qt using the component interface
  -> staged Qt/QML/runtime/helper artifacts
  -> tests against that stage
  -> platform package + checksums/provenance
```

The local upstream Makefile currently uses `with_gvisor`, sets `CGO_ENABLED=0`, and
has architecture-specific targets. Several generic amd64 targets select GOAMD64=v3;
choose an explicit v1 baseline for broad initial x86_64 desktop compatibility unless
the release support policy says otherwise. Its build time comes from wall-clock
`date`; reproducible builds need controlled metadata rather than claiming identical
hashes merely because the source commit is fixed. Evaluate any tag changes in tests.

Use a small project-owned Go build wrapper with explicit equivalent flags and output
directories, or invoke verified upstream targets without modifying the submodule's
Makefile. Keep products under `build/<preset>/`, not source paths. Propagate config,
architecture and source changes into the build dependency graph; never reuse a
different target's binary accidentally. No silent binary-download fallback.

Expected files:

| File/directory | Purpose / owner |
| --- | --- |
| Root `Makefile` | Developer command facade, coordinator-owned |
| `CMakePresets.json` | Checked-in configure/build/test presets; exclude private local paths |
| `cmake/Mihomo.cmake` | Source build/imported executable/staging integration |
| `cmake/Packaging.cmake` | Shared package assembly and runtime dependency handling |
| `scripts/build/` | Small platform-neutral orchestration plus explicit platform helpers |
| `packaging/macos/` | Bundle/DMG assets and signing/notarization automation |
| `packaging/windows/` | ZIP/installer/component DLL deployment manifests and scripts |
| `packaging/linux/` | Portable package, desktop integration and later distro manifests |
| `.github/workflows/` | Native OS jobs invoking the same Make facade |

GNU Make is an explicit prerequisite, not NMake. For Windows, document one tested
launcher environment with GNU Make plus an initialized MSVC/Windows SDK/Qt toolchain;
build recipes delegate portable work to CMake/Python and avoid relying on `/bin/sh`.
Pin and validate this environment in CI before publishing commands as supported.
Linux/macOS use their documented native toolchains. Python is a declared dependency
if the wrapper uses it; do not assume it exists on a clean Windows machine.

`make doctor` verifies tools/architecture/submodule readiness without installing
anything. `make setup` is the explicit dependency acquisition step. `make run`
resolves the staged component/core, and `make test-integration` builds that core
before testing. Runtime discovery of unrelated installed binaries cannot satisfy a
managed build/test requirement. All test and package commands return nonzero on
failure; optional unsupported cases are reported rather than masked.

## Platform packages and validation

| Platform | Initial architectures | Package and native requirements |
| --- | --- | --- |
| macOS | arm64, x86_64 as separately tested artifacts | `.app` + DMG; component/core under bundle-owned paths; Qt/QML/frameworks; helper placement; sign nested binaries, then bundle, then notarize/staple for public release |
| Windows | x86_64; arm64 later | Portable ZIP plus installer; portable component DLL and Qt/QML/runtime DLLs; loader/ABI checks; Authenticode for public releases |
| Linux | x86_64; arm64 later | Start with relocatable tarball, add AppImage in a tested runtime baseline; desktop/icon integration, loader paths/Qt plugins; explicit supported distro baseline and X11/Wayland feature matrix |

Architecture expansion requires a real runner and compatible dependencies, not just
adding a Go cross-build target. Set minimum OS/libc/compiler versions from an actual
Qt dependency inventory. Do not advertise a universal macOS artifact until every
embedded component supports both slices. Helper elevation/service features have
separate native gates; unsupported TUN/hotkey behavior is clearly surfaced.

Packages must include the required Qt runtime/plugins/QML modules or declare a tested
system dependency strategy. Native deployment differs by OS; see
[Qt deployment guidance](https://doc.qt.io/qt-6/deployment.html). Test from a relocated
installation without the build-tree environment or developer SDK paths. Verify
component version/loading, real local HTTP proxy traffic, settings persistence,
QML/icons, native controls, normal quit and no remaining owned engine process.

Windows baseline uses the reference-inspired portable component loader, without
native Microsoft COM registration. If a native bridge is added separately, its
registration/runtime requirements belong to that extension's own package and tests.
Installer upgrade/removal preserves user data unless the user requests removal.

Emit app/component/core versions, source commit, toolchain, build tags, target and
checksums in artifact metadata. Include required notices and source provenance.
Signing secrets stay in CI credential storage. Unsigned development packages can be
tested before credentials exist but cannot pass the signed-release gate.

## Native CI and parallel ownership

Each native job runs `doctor`, configure/build, feature/architecture tests, local-core
integration, package assembly, and clean-install smoke as applicable. Provision
dependencies ahead of test execution; use architecture/toolchain-specific caches.
Platform packaging workers own disjoint `packaging/<os>/` and corresponding tests;
the coordinator owns Make, presets, central workflows and aggregate manifests.

Schedule build facade/local-core implementation in P2 as coordinator work or a bounded
BUILD package that replaces one worker slot. Do not add a fourth worker. In P7,
subdivide QUAL-NATIVE into macOS/Windows/Linux packages and use available slots in
batches. Each gets its own host/build/install root. Native networking/service tests
use exclusive disposable environments and never the user's active proxy/VPN state.

Local-core build must feed portable COM module tests on all three OSes. A fake backend is
useful contract coverage but cannot close the source-built/native release gates.

## Monitor and MITM add-on builds

`make capture-addons` builds selected add-ons without making them mandatory for
ordinary proxy use. Pin `3rdparty/mitmproxy` to the recorded local commit; build its
project-owned worker package against that source, provision Python >=3.12 and the
compatible native mitmproxy_rs/helper dependencies, and record hashes/versions in
the package manifest. Audit Python/native-wheel/platform compatibility on each OS;
do not silently use a globally installed mitmdump executable or the latest container.

Keep project bridge/add-on scripts in `src/services/capture/mitmproxy/`, not patched
into the upstream checkout. Build the monitor container from `services/monitor/`
with pinned base/runtime dependencies and tag it by source provenance/digest. Package
sensor modules/helpers and optional container assets under `packaging/capture/`.
Native host targeting/trust adapters remain outside the analytics container. Share
only explicit data/event channels and owned volumes, not arbitrary host directories.

Document optional runtime installation, image import/pull policy, offline prerequisites,
capture storage, CA/trust lifecycle, uninstall and scheduled-job service setup. Build/
install never starts capture or trusts a CA automatically. Full add-on qualification
includes actual native process selection and disposable trust/network tests; a running
container or Python import is not sufficient evidence. Missing optional dependencies
leave the corresponding add-on unavailable without preventing the app from starting.

`make test-capture` runs synthetic client/server workflows and module/store/scheduler
contracts. Native capture cases remain opt-in. If upstream mitmproxy tests or files
are touched, follow its AGENTS.md: `uv run pytest`, `uv run tox`, and individual
coverage for new upstream files. No upstream files are modified in this planning pass.

The legacy-v1 preservation inventory includes both source-submodule references and
the user-provided COM reference needed to understand the pre-refactor plans.

## Archival and main cutover checklist

This is a future coordinator-owned change after the user-requested refactor starts:

1. Inventory tracked, staged, untracked docs/code and submodule state. Prepare an
   explicit reviewed pre-refactor snapshot; include existing planning/audit docs and
   necessary build/source references, without blanket staging unrelated/generated data.
2. Create `legacy-v1` at that committed snapshot. Verify its tree includes the old
   docs and the current code; verify submodule provenance/retrievability. If the branch
   already exists, inspect it and preserve it instead of silently overwriting it.
3. Branch `codex/refactor-v2` from the snapshot and implement the dependency-gated
   packages. Main remains a usable baseline until cutover.
4. After G1/G2/G3/G5/G6 foundation acceptance, write fresh root README and new docs:
   `docs/architecture.md`, `docs/build.md`, `docs/development.md`, `docs/testing.md`,
   `docs/packaging.md`, `docs/module-api.md`, and `docs/migration.md`. Final scope
   can consolidate these if shorter docs suffice. Document actual commands/features.
5. Remove all old pre-refactor docs from the new branch via an explicit reviewed list,
   including this planning set; legacy-v1 is their archive. Check every remaining
   link, command, platform claim and branch reference.
6. Integrate intervening main changes and rerun affected acceptance against the exact
   release candidate. Preserve `legacy-v1` in the intended remote before publishing
   the cleaned main when remote publishing is part of the execution request.
7. Merge normally into `main` through the repository's review workflow, without
   force-pushing/replacing history. Keep `legacy-v1` as the frozen historical branch.

No deletion or branch movement occurs in this goal-setting pass. Cleanup is blocked
by an unverified archive or incomplete foundation release gates, not by optional
later business/VPN/plugin features.
