# Sub-agent execution plan

Status: proposed, 2026-09-21. This reorganizes the implementation roadmap; it does
not authorize starting all implementation packages during the planning pass.

[Project goals G1–G8](PROJECT_GOALS.md) now govern delivery. In P0 the coordinator
must preserve old code/docs on `legacy-v1` before P1 source moves. In P2 it owns
Make/presets and local mihomo source-build integration, or delegates a bounded BUILD
package by replacing a worker slot. Required portable-COM module/platform validation
precedes the foundation DOCS-CUTOVER gate; optional later features need not block it.
The COM model follows `3rdparty/ref/fxcom` on all platforms, without its FX naming.
Native Microsoft COM is optional. Requested monitor/MITM scope is specified in
[CAPTURE_MODULE_PLAN.md](CAPTURE_MODULE_PLAN.md); capture waves can also use freed
slots after their actual prerequisites land.

## Team and scheduling

Use **one coordinator and up to three concurrent workers**, matching the current
four-agent capacity. Workers receive bounded packages, not permanent ownership of
an entire product track. A track is split into sequential packages where stated.
The coordinator does useful local integration/build/review work while workers run.

The [roadmap](IMPLEMENTATION_ROADMAP.md) stages retain product scope and acceptance
criteria. The wave/package IDs below govern dispatch and dependencies. The source
plan remains authoritative for existing-file destinations; the roadmap lists new
code destinations. All four supporting plans remain applicable.

Waves are scheduling groups, not a requirement that unrelated work wait. A successor
can start when its prerequisites are integrated, its interfaces are reviewed, and
its file scope is free. **Relocation and shared build changes have mandatory whole-
tree barriers.** No dependent package consumes a merely promised predecessor.

| Wave | Worker slot A | Worker slot B | Worker slot C | Coordinator and gate |
| --- | --- | --- | --- | --- |
| P0 Preparation | PRE-ARCH: dependency/ownership inventory and proposed contracts | PRE-TEST: F01–F10/W01–W05 coverage and independent fixtures | PRE-MOVE: exact move map and resource/package hazards | Record dirty-tree/submodule state, run baseline, reconcile contracts and publish dispatch scopes |
| P1 Relocation | MOVE-UI: UI files/resources | MOVE-CORE: core/platform/helper files | MOVE-TEST: existing suites intact | Own cross-scope includes and all manifests; fresh build, unchanged assertions/inventory, resource/helper smoke |
| P2 Build and test foundations | BASE-FIXTURE: scoped settings, local servers, portable fake executable | BASE-SEAM: proxy/browser/deadline injection and affected tests | BASE-ARCH: dependency checker and checker self-tests | Extract libraries/headless configuration, partition legacy suites, integrate target registrations; architecture and existing feature checks |
| P3 Portable application | MOD-CORE: MihomoBackend and adapter contracts | MOD-RUNTIME: runtime/routing coordinator and tests | MOD-LIFECYCLE: shutdown/backup coordinator and tests | Publish backend/coordination contracts, wire main/context/UI, run combined lifecycle and ownership workflows |
| P4 Configuration and component ABI | CFG-CORE: composer extraction, preset semantics/storage, profile integration | CFG-UI: preset controls and effective-config presentation | COMPONENT-ABI: module factory/loading/lifetime and sample consumer | Integrate preview/application, validate W02 and shared-library ABI on each native target |
| P5 Network and identity | NET: registry, planner and Tailscale track | IDENTITY: OIDC/session/credential adapters | POLICY: management API, policy client and enforcement track | Freeze policy/network schemas; coordinate cross-scope integration and test login→enroll→apply→ack plus network conflicts |
| P6 Product integrations | VPN: EasyConnect endpoint, then container/Fudan integration | USAGE: durable reporting and administrative views | COMPONENT-PACKAGE: portable module distribution integration | Verify policy guards at all implemented entry points, usage coverage, owned resources and shared backend contracts |
| P7 Extension and qualification | EXT: external plugin host/protocol/example | QUAL-PORTABLE: workflows, installed artifacts, real-core checks | QUAL-NATIVE: native display/network/component matrix | Publish immutable candidates, verify plugin policy enforcement/bypass cases, integrate fixes and aggregate platform evidence |
| P8 Capture providers/data | MON: host sensor + container monitor backend | MITM: local-source worker/adapter | CAP-DATA: catalog/export/scheduler | CAP-0 contracts and host coordinator integrated first; attribution/completeness gates |
| P9 Capture experience/release | CAP-UI: selection/live/content/export/jobs UI | CAP-NATIVE: target/trust/routing support | CAP-PACKAGE: optional artifacts and workflows | Integrate real providers/native evidence; coordinator owns shared network/trust/shell wiring |

Within any wave, preserve the four-slot limit. For example, an independent workflow
review replaces a finished worker; it is not an extra fifth agent. A worker needing
an unavailable Windows/native environment reports the exact missing evidence and
releases the slot; unrelated ready packages continue. The platform gate remains open.

## Coordinator-only surfaces

The coordinator is the only writer to these files during implementation waves:

- Root `CMakeLists.txt`, shared `sources.cmake`/assembly definitions,
  `tests/CMakeLists.txt`, `cmake/Packaging.cmake`, `cmake/TrafficGraph.cmake`, central
  presets/CI configuration, root `Makefile`, `cmake/Mihomo.cmake`, `scripts/build/`
  and shared build helper files.
- `src/main.cpp`, `src/app/app_context.h` (and its old location during movement),
  application composition and cross-feature shell wiring.
- Published contracts under `src/core/backend/`, shared value/schema definitions,
  portable binary contracts under `src/core/component/`, and each wave's agreed
  cross-component interfaces.
- Plan/status documents, test coverage index, and the ownership ledger.

Workers send exact source/target/link/label registration requests instead of editing
central CMake. New module-local build files can be delegated explicitly, but their
parent registration remains coordinator-owned. Shared contracts may be drafted by
a worker for review; only the coordinator applies cross-consumer changes.

This is integration ownership, not permission for the coordinator to edit worker-
owned files concurrently. Wait for a handoff or agree an explicit ownership transfer.
When needed, dispatch a bounded build package and transfer its exact paths temporarily.

## Exclusive implementation scopes

Every dispatch names concrete files within the scopes below, excluding coordinator
surfaces. `**` describes a possible ownership envelope, not permission to rewrite
all files there. A feature owner also owns its named adjacent tests.

| Package | Allowed implementation scope | Tests / handoff |
| --- | --- | --- |
| MOVE-UI | `src/ui/**`, including destinations in source plan | Moves only; report external include/QML/QRC references |
| MOVE-CORE | `src/core/**`, `src/platform/**`, `src/service/macos_helper.mm` → `src/services/macos/` | Moves only; report source/framework/helper-path changes |
| MOVE-TEST | Existing `tests/*.cpp` → `core/`, `platform/`, `ui/` | Preserve every case and test environment |
| BASE-FIXTURE | `tests/support/**`, assigned synthetic `tests/fixtures/**` | Fixture tests and stable minimal APIs |
| BASE-SEAM | Named proxy/browser/helper-client files; `ui/shell/dashboard_button.*`; deadline injection here is for the helper client | Named proxy/dashboard/helper-client suites; no global fixture or public-contract edits |
| BASE-ARCH | `tests/architecture/**` and private checker fixtures | Checker positive/negative cases; request headless/header build probes |
| COMPONENT-BASE | Explicit lease for `src/core/component/**` | `tests/contracts/component/**`; object/query/result/refcount audit; freed P2 slot before P3 |
| MOD-CORE | `src/core/mihomo/**`, portable loader `src/integrations/component/**`, including any CoreProcess readiness/deadline seam needed by its tests | Module/sample-consumer tests and `tests/core/mihomo/**`; proposed contract cases to coordinator |
| MOD-RUNTIME | `src/app/runtime/**` | `tests/app/runtime/**`, `tests/app/routing/**` |
| MOD-LIFECYCLE | `src/app/lifecycle/**`, `src/app/backup/**` | `tests/app/lifecycle/**`, `tests/app/backup/**` |
| CFG-CORE | `src/core/config/**`, `src/core/profiles/**` | `tests/core/config/**`, `tests/core/profiles/**` |
| CFG-UI | Named new preset/preview widgets in `ui/pages/settings/` and `profiles/` | Matching `tests/ui/pages/**`; coordinator wires shared existing pages |
| COMPONENT-ABI / COMPONENT-PACKAGE | Handed-off component loader/module files, independent sample consumer and named platform package files | Shared-library/version/lifetime/native-target tests; central registration requests |
| NET | `core/network/**`, `app/network/**`, `integrations/network/tailscale/**`, built-in `extensions/` registry; NET-3 additionally owns named `platform/network/**` route/DNS adapters and explicitly leased proxy-adapter files | Network/Tailscale and named platform-adapter tests; explicit registry handoff before EXT |
| IDENTITY | `app/identity/**`, `integrations/identity/**`, `platform/credentials/**` | Identity/session/credential-adapter suites |
| POLICY | `core/policy/**`, `integrations/management/**`, `management/api/**` | Policy/client/server tests; guarded-operation requirements to coordinator |
| VPN | `integrations/network/easyconnect/**` | Endpoint/container tests and campus validation report |
| USAGE | `app/usage/**`, explicitly transferred management transport/API files, `management/admin/**` | Outbox/attribution/admin tests; obtain POLICY handoff first |
| EXT | Explicitly transferred `src/extensions/**`, example extension and SDK docs | Host/provider compatibility and crash/version tests |
| QUAL-PORTABLE | `tests/workflows/**`, `tests/packaging/**`, assigned real-core contract drivers | Integrated journeys and installed artifact evidence |
| QUAL-NATIVE | `tests/native/**`, `tests/benchmarks/**`, assigned Windows qualification tests | Matrix of pass/fail/skipped with OS/core/Qt/build revisions |
| MON | `src/integrations/capture/monitor/**`, named `src/platform/capture/**`, repository-root `services/monitor/**` | Monitor/attribution/container tests |
| MITM | `src/integrations/capture/mitmproxy/**`, `src/services/capture/mitmproxy/**` | Worker/protocol/flow-save tests; no upstream edits |
| CAP-DATA | `src/core/capture/store/**`, `src/app/capture/schedules/**` | Catalog/export/quota/scheduler tests |
| CAP-UI | `src/ui/pages/capture/**` | Selector/monitor/inspector/export/jobs tests |
| CAP-NATIVE | Transferred `src/platform/capture/**`, `src/platform/trust/**`, named provider files | Target/trust/coexistence on disposable hosts |
| CAP-PACKAGE | `packaging/capture/**`, named capture workflow/install tests | Source/runtime/image provenance and install evidence |

Abbreviated production source paths such as `core/` and `app/` are beneath `src/`;
`management/api/`, `management/admin/`, `services/monitor/`, and `tests/` are repository-root paths.
No worker package edits `3rdparty/mihomo`, `.gitmodules`, the Git index, or an
unrelated staged change without an explicit ownership transfer. Build workers may
compile the recorded local checkout into isolated output directories; do not advance
its ref. The coordinator records its provenance in the archival snapshot and build
manifest. Submodule existence is not evidence the produced binary passed tests.
The same read-only source rule applies to `3rdparty/mitmproxy` and `3rdparty/ref/fxcom`.
Build project adapters outside them; follow the COM plan's provenance/audit rules and
never propagate FX API naming into the new component.

`ProfileStore` remains owned by CFG-CORE during composition extraction; other workers
consume its reviewed public API. Existing settings/profile pages and routing/shell
entry points are coordinator integration files after P1, unless a later dispatch
explicitly leases them to one worker. Do not have several agents edit different
methods in the same file at once.

## Contract gates and smaller packages

Before P3, COMPONENT-BASE delivers the audited reference-inspired object/query/reference
foundation with project-owned names, using a freed P2 slot. Settle lifecycle ownership, request acceptance/completion, cancellation,
generations, terminal states and desired/applied revisions. Define which coordinator
owns transitions: runtime manages execution, routing submits intent, backup requests
maintenance, and shutdown waits for documented terminal outcomes. This prevents
runtime and backup agents from independently designing contradictory state machines.

As the P3 dispatch gate, the coordinator implements and validates a small in-process
fake backend under `tests/support/backend/` against these reviewed contracts, with
contract tests under `tests/contracts/`. This is distinct from BASE-FIXTURE's fake
executable. Transfer these exact fixture paths from the earlier support owner first.
MOD-RUNTIME, MOD-LIFECYCLE and COMPONENT-ABI consume this handed-off fake; they must not
invent separate incompatible versions. Their passing fake tests remain separate
from proof that the real MihomoBackend implements the contract.

Before P4, settle immutable composition input/result, provenance, diagnostics,
reserved settings and legacy/new preset semantics. CFG-UI may develop presentation
against agreed synthetic results while CFG-CORE implements them. It cannot claim
the workflow passes until real composition is integrated. Component ABI qualification
starts with the P3 fake backend; published configuration interfaces wait for P4.

Before P5, settle network observation/requirement/plan schemas, policy envelope and
management API/authentication boundaries, identity subject mapping and the authority
that checks managed actions. Coordinator applies policy hooks to runtime/components/UI;
POLICY does not independently edit those packages' implementations.

Large tracks above must be dispatched as smaller bounded packages in order:

- **NET-1 → NET-2 → NET-3:** read-only Tailscale observations; pure combined planning;
  OS application/recovery with native tests. No simultaneous host-network writers.
- **IDENTITY-1 → IDENTITY-2:** OIDC/session/credential contracts; verified Casdoor and
  Feishu deployment flow. Missing real credentials do not block local protocol tests
  or count as completed deployment validation.
- **POLICY-1 → POLICY-2 → POLICY-3:** schema/API/auth and test endpoint; desktop policy
  fetch/cache; enforcement and applied-revision acknowledgement. Thin assignment
  APIs precede full administrative screens. Select server stack before POLICY-1.
- **VPN-1 → VPN-2:** existing endpoint/DNS integration; owned container lifecycle and
  Fudan preset. NET schemas and integrated host planner must exist first.
- **USAGE-1 → USAGE-2:** counters/outbox and idempotent server ingestion; administration
  and reporting. Contract fixtures can be drafted earlier, but files/API ownership
  transfer from POLICY before implementation starts.
- **COMPONENT-BASE → MOD-CORE → COMPONENT-ABI → COMPONENT-PACKAGE:** audited object
  foundation, module implementation, factory/loader qualification and native package
  proof on all OSes. Managed component access also depends on POLICY-3 authority
  enforcement. Native Microsoft COM is a separately scoped optional bridge.
- **EXT-1 → EXT-2:** process protocol/host; independent example, packaging and host
  recovery. Both built-in integrations must have exercised the contracts first.

Freeze a reviewed working contract revision per dispatch, not an irreversible ABI
for hypothetical future features. A worker proposing a change reports affected
consumers; coordinator reviews/applies it, updates the revision, and reassigns any
dependent work. Published COM/protocol compatibility follows the dedicated plan.

## Test ownership and integration

Every implementation package includes normal behavior, key failures and recovery
tests. There is no downstream agent whose job is to retrofit all feature tests.
An independent validation package covers cross-component expectations and reviews
test oracles. The coordinator owns `tests/contracts/**` until explicitly assigning
non-overlapping files to a contract worker.

PRE-TEST prepares inventories, scenario specifications and hand-authored expected
examples. BASE-FIXTURE owns local servers, fake executables and their utility tests.
BASE-ARCH alone owns checker self-tests and checker fixtures. Headless builds,
public-header consumers, backend replacement, coordinator tests and complete app
journeys depend on actual integrated target/interface definitions. No passing
placeholder suites stand in for missing implementations.

Move `runtime_test.cpp` and `data_pages_test.cpp` intact in P1. Before P3, the
coordinator (or one explicitly delegated splitter) partitions them using a per-case
preservation map. Do not give portions of a still-shared suite to multiple writers.
Retain ordinary behavior and distinct regressions; record replacements/deletions in
the coverage index. Move native frame measurement into the benchmark lane separately.

After each handoff, the coordinator reviews changes, applies registrations/wiring,
and tests the assembled result. Gates include relevant existing suites, public
contracts, targeted cross-boundary checks and cleanup. Full existing suites run at
major combined milestones. Branch-local passing tests alone never close a wave.

## Shared workspace and integration discipline

Default to the shared workspace with an explicit file-ownership ledger: package,
owner, exact paths, input contract revision, status and successor. Workers do not
stage/commit/reset/rebase or reformat other scopes. The coordinator alone handles
Git integration and only stages task-owned paths when committing is requested.
Unrelated user edits, including staged submodule changes, must remain intact.

During P1, each worker fixes references within its own files and reports cross-scope
edits. The coordinator performs global reference/manifests reconciliation after all
moves are handed off. The tree may temporarily be unbuildable; no dependent worker
starts against that intermediate tree. P2 shared-target rewiring uses the same rule.

If isolated worktrees are useful, first establish an explicit baseline containing
the authorized current work. A worktree at HEAD does not automatically include
uncommitted changes. Do not silently drop them, commit unrelated changes, or create
overlapping worktrees as a substitute for scope ownership. Integrate independent
patches in dependency order and rerun combined checks.

Assign unique build/data/install roots per package (for example
`build-agent-mod-core/`); a build directory does not isolate shared source mutations.
The coordinator grants stable-source build windows or uses an agreed snapshot.
Use unique sockets/ephemeral ports and clean fixture children. Native proxy/TUN/helper
tests require an exclusive disposable-host lease. Do not mutate the user's live
networking or let multiple workers operate one native environment concurrently.

When a package fails review or integration, return the failure to its scope owner
with reproducible evidence. Do not launch a second writer on the same files. The
coordinator may integrate unrelated completed packages while the fix is pending.

## Dispatch and handoff templates

Dispatch each task with this information, expanded to actual filenames:

```text
Package: MOD-CORE
Objective: Wrap the existing mihomo implementation behind the reviewed contracts.
Inputs: Integrated P2 tree; exact backend contract revision; ownership decisions.
Own: Enumerated src/core/mihomo and tests/core/mihomo files.
Do not edit: shared contracts, main/context, UI, CMake, other tests, Git index.
Behavior: asynchronous accepted/completed operations; managed/external distinction;
          stale-result rejection and cleanup outcomes preserved.
Verification: named feature/adapter tests; real-core checks if provisioned.
Deliver: implementation/tests plus precise central registration/wiring requests.
Escalate: contract changes, out-of-scope files, unavailable platform evidence.
```

Every worker returns: changed files, contract revision consumed, behavior delivered,
test commands/results including failures/skips, resources cleaned up, remaining
limitations, and exact integration requests. Report code ready versus integrated
and verified separately. Integration gates are recorded by the coordinator only.

The first implementation dispatch should be **P0's three read-only review packages**
while the coordinator establishes the baseline, followed by **P1's three disjoint
move packages**. Do not start all feature tracks from the current flat tree.

## Build, platform and documentation completion packages

**BUILD** owns only coordinator-leased build facade/source-build files and their
smoke tests. It implements the Make command contract and stages the locally built
core/module/app. It replaces a P2 worker slot when delegated. Architecture workers
consume its manifest/preset contracts after handoff rather than inventing new ones.

**PACKAGE-MAC / PACKAGE-WIN / PACKAGE-LINUX** own disjoint `packaging/<os>/` and named
native/package tests. Run in batches within the three-worker limit and on native
hosts; coordinator integrates central CI/presets. Use [BUILD_RELEASE_PLAN.md](BUILD_RELEASE_PLAN.md)
for clean-install, signing, module activation and local-core provenance gates.

**DOCS-CUTOVER**, coordinator-owned after G1–G6 foundation acceptance, verifies the
legacy-v1 snapshot, writes fresh implementation documentation, removes all old docs
from the refactor branch by explicit reviewed list, and normally merges validated
refactored code into main. It does not reset/force-push history or delete legacy-v1.
See the build/release plan for exact sequencing and remote-preservation requirements.


## Capture prerequisite and ownership gates

CAP-0 is a bounded preparatory package in a free worker slot after component/runtime/
network contracts land. It drafts capabilities, stable target identity, sessions,
events, artifacts and schedule schemas plus platform feasibility results. Coordinator
publishes the reviewed contracts under `src/core/capture/` and component interfaces,
and integrates `src/app/capture/capture_coordinator.*` and `target_resolver.*` before
P8. A delegated implementation of these host services gets an explicit exclusive
lease. Monitor and MITM providers may not each invent a separate process resolver.

P8 workers own distinct provider and storage paths. They consume shared fake events
and hand-authored flows. MON sensors initially observe only; interception/trust work
belongs to MITM/CAP-NATIVE after an explicit platform-file transfer. CAP-DATA owns
scheduler/store implementation, not provider internals or shared contracts. Parent
CMake/Make, central Python/image manifests and shell/navigation wiring remain with
the coordinator. Common container-runtime code is coordinated with VPN rather than
duplicated or edited concurrently.

P9 starts CAP-UI after catalog/provider contracts are integrated, CAP-NATIVE after
provider handoff, and CAP-PACKAGE after source/runtime manifests exist. Split native
work into OS-specific packages within the same three-slot capacity. Containerized
metadata monitoring does not prove host-app attribution; fake TLS fixtures do not
prove native per-app interception. Include inbound server versus outbound replies,
PID reuse, encrypted visibility, script/schedule recovery and storage completeness
in combined gates. No worker modifies a user's active trust store or network for CI.

Capture modules use the built-in portable COM SDK first, so P8/P9 do not depend on
EXT completion or all managed features. When managed mode is enabled, coordinator
adds authorization constraints at capture/schedule entry points. G7/G8 completion is
tracked separately from the G1–G6 refactor/main cutover; no empty future tests count.
