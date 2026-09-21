# Parallel implementation roadmap

Status: proposed, 2026-09-21. No implementation is performed by this document.

[Project goals G1–G8](PROJECT_GOALS.md) are the governing acceptance criteria:
local-source mihomo, a required separate COM/component boundary, documented Make
commands, legacy-v1 preservation and docs replacement, native platform packaging,
and architecture/feature/workflow tests. [BUILD_RELEASE_PLAN.md](BUILD_RELEASE_PLAN.md)
defines the build and cutover deliverables. These supersede earlier optional COM
scope or prebuilt-engine assumptions. The COM model now follows the object/interface
design in `3rdparty/ref/fxcom` without FX naming, on all three platforms; native
Microsoft COM is optional. G7/G8 add the [monitor and MITM modules](CAPTURE_MODULE_PLAN.md).

## Scope and source of truth

This roadmap supplies the implementation order across four plans:

| Plan | Responsibility |
| --- | --- |
| [Architecture and management](MODULAR_MANAGEMENT_PLAN.md) | Feature behavior, configuration precedence, identity, policy, usage and VPN integration |
| [Mihomo component and COM](COM_MODULE_PLAN.md) | Reference-inspired portable COM contracts, module loading and ABI qualification |
| [Source refactor](STRUCTURE_REFACTOR_PLAN.md) | Exact existing-file destinations and migration hazards |
| [Test redesign](TEST_STRATEGY.md) | Architecture checks, feature contracts F01–F10, workflows W01–W05, fixtures and CI |
| [Monitor and MITM](CAPTURE_MODULE_PLAN.md) | App targeting, container monitoring, source-based mitmproxy, capture storage/export and periodic scripts |

Use the [sub-agent execution plan](PARALLEL_EXECUTION_PLAN.md) for dispatch: one
coordinator and up to three workers, exclusive file scopes, contract revisions and
combined-tree gates. Its P0–P9 waves/package IDs determine execution; the stage
numbers below retain product-scope traceability. Earlier documents' local phases
are descriptive rather than competing schedules. Use the source-refactor tree as the immediate physical
layout, with new locations listed below. Do not also introduce the architecture
proposal's earlier `domain/` and `application/` trees: pure logic grows under `core/`
and coordination under `app/`. This avoids moving the same responsibility twice.

Each stage can contain several reviewable changes. File relocation, behavior
extraction, and new features should be separate changes. Tests ship with each stage;
test redesign is not a final cleanup task. Preserve existing uncommitted work and
review the live tree before making moves; the working tree is actively changing.

## Parallel schedule

| Wave | Three independent workstreams | Product stages |
| --- | --- | --- |
| P0 | Architecture inventory; test/coverage inventory; move/package inventory; coordinator preserves pre-refactor legacy-v1 snapshot before code moves | 0 |
| P1 | UI moves; backend/platform/helper moves; test moves | 1 |
| P2 | Fixtures; narrow production/test seams; architecture checks, with a bounded COMPONENT-BASE package plus coordinator-owned build wiring | 2, C |
| P3 | Separate COM-style Mihomo module; runtime/routing services; shutdown/backup services | 3 |
| P4 | Configuration/presets; preset/preview UI against agreed contracts; portable component factory/ABI qualification | 4, C |
| P5 | Network/Tailscale; identity/credentials; policy service/client/enforcement | 5, 6 |
| P6 | EasyConnect; usage/administration; portable module package integration | 7, 8, C |
| P7 | External plugin SDK; portable workflows/package qualification; native qualification in OS-specific packages | 8, 9, C |
| P8 | Monitor sensor/container backend; mitmproxy worker backend; shared capture store/export/scheduler, after CAP-0 contracts | 10, 11 |
| P9 | Capture UI; native targeting/trust/coexistence; add-on builds/package/workflow qualification | 10, 11, 12 |

Archive current code/docs on `legacy-v1` before P1. Foundation acceptance for G1–G6
can schedule a separate DOCS-CUTOVER gate to replace old docs and merge the refactor
into `main`; it does not wait for optional business/VPN/plugin product tracks.

The coordinator owns central build files, main/context, shared contracts and final
wiring. Each feature worker owns implementation plus its tests. Each broad track
is dispatched as bounded subpackages; exact scopes, prerequisites and handoff
templates are defined in [PARALLEL_EXECUTION_PLAN.md](PARALLEL_EXECUTION_PLAN.md).
Relocation/build barriers are mandatory. Later successors may start as soon as
their actual prerequisites are integrated and file ownership is available.

## Product stages and acceptance gates

| Stage | Implementation | Acceptance gate |
| --- | --- | --- |
| 0. Baseline and contracts | Build current tree, record tests/skips/platforms; index current feature coverage; document ownership and configuration contracts | Reproducible baseline with failures recorded; no unexplained behavior change mixed into refactoring |
| 1. File organization | Apply the exact movement map; update includes, manifests, QRC/QML paths, helper paths and documentation | Fresh build and existing tests; QML/icons load; helper/package paths resolve; no API or data-format changes |
| 2. Libraries and test infrastructure | Extract reusable backend targets and build helpers; introduce narrow test seams and scoped fixtures; add architecture checks | Tests use production targets; backend config/tests configure without desktop Qt modules; forbidden dependencies fail checks |
| 3. Application ownership and Mihomo backend | Extract runtime/routing/shutdown/backup coordination and portable core contracts; wrap existing process/controller/provider implementations | App services run with a fake backend; real backend passes lifecycle/control contracts; W01/W03/W04/W05 exercise assembled behavior |
| 4. Configuration pipeline and local presets | Extract pure composition and runtime I/O; add global/per-profile presets, explicit operations, provenance and effective-config preview | Legacy behavior retained; subscription refresh preserves overrides; invalid candidates preserve applied state; W02 passes with pinned real-core smoke |
| 5. Network extension foundation and Tailscale | Built-in registry, observations, network planner and conflict diagnostics; add read-only detection first, then opt-in routing/DNS coexistence | Absent/offline/connected states work; conflicts are visible; native System Proxy/TUN combinations pass on each claimed platform |
| 6. Identity and managed policy | OIDC/Casdoor with Feishu federation; secure session storage; enrollment and management API policy delivery; enforce policy at application boundary | Tenant/session isolation, offline/expiry behavior and normal login work; UI/tray/import/restore/live changes cannot bypass managed constraints |
| 7. Administration and usage | Member/device/preset assignments, policy acknowledgement, durable usage collection/outbox and reporting UI | Per-device attribution, retry deduplication and counter resets/gaps verified; coverage and applied-policy status are visible |
| 8. EasyConnect and external extensions | Local endpoint adapter, then container lifecycle and Fudan preset; extract external plugin protocol from working integrations | Campus DNS/resources tested; container ownership/recovery verified; independent example extension requires no host source edits |
| 9. Release qualification | Complete native/platform matrix, installed-artifact checks, sustained lifecycle tests and supported performance budgets | Supported combinations have evidence; unsupported/skipped cases are documented; no owned-resource leaks in normal tested shutdown |
| 10. Application monitor | Selected-app host sensors and containerized metadata analysis/storage | Qualified inbound/outbound endpoints/bytes, observed URL provenance, correct app attribution and explicit gaps |
| 11. MITM inspection | Local mitmproxy source worker, app selection, trust readiness, request/reply decoding and forwarding | Qualified HTTP/TLS/content fidelity, opaque/pinned/unsupported states, no unrelated interception and cleanup |
| 12. Capture workflows | Shared catalog/export, periodic jobs/scripts, UI and optional native packages | Saved/exported captures reopen; quotas/completeness and schedule outcomes accurate; native app/mode matrix qualified |

Track C is the required portable COM foundation, following the user's fxcom reference
without its FX naming. COMPONENT-BASE lands before P3 consumers; COMPONENT-ABI tests
shared-library loading/factory/lifetime on each OS; COMPONENT-PACKAGE qualifies the
installed module. Native Microsoft COM is a separate optional interoperability task.
See [the revised COM plan](COM_MODULE_PLAN.md).

CAP-0 freezes target/session/capability/storage/IPC contracts and records native
capture feasibility before P8. P8/P9 can use freed earlier slots after component,
runtime and network contracts are integrated; they do not require management or the
external plugin SDK. Preserve the three-worker limit and native-host leases.

## Concrete work packages

### Stage 0 — Establish behavior before movement

Record the current CTest inventory rather than assuming the previous count still
holds. Run the full available suite and fresh build without resetting active work.
Commit the reviewed pre-refactor snapshot, including existing docs and local-core
provenance, when implementation begins; create `legacy-v1` and the refactor branch
according to the [migration checklist](BUILD_RELEASE_PLAN.md#archival-and-main-cutover-checklist).
Create `tests/README.md` with feature coverage IDs, test ownership, known skips and
commands. Define managed versus external core ownership, desired versus applied
state, and current override/script behavior. Capture synthetic reusable fixtures;
do not snapshot user subscriptions or credentials.

### Stage 1 — Make the tree readable

Use [the complete destination table](STRUCTURE_REFACTOR_PLAN.md#exact-file-movement-for-the-first-phase).
Make UI relocation one change and backend/platform/test relocation another. Preserve
filenames, namespaces, QObject ownership and signals. In particular:

- Shell/tray/routing widgets go to `src/ui/shell/`.
- Pages go to `src/ui/pages/<feature>/`; graph/QML stay with `overview`.
- Shared controls, theme and resources get their own UI folders.
- Mihomo code goes to `src/core/mihomo/`, including its process subfolder.
- Config, profiles, backups and telemetry get focused core folders.
- The helper moves to `src/services/macos/`; browser code to `platform/browser/`.
- Existing tests first move into `tests/core/`, `tests/platform/`, `tests/ui/`.

Do not split suites/classes while moving them. Preserve QML URI/type names, QRC
public URLs, settings keys, backup formats and helper protocol. Existing `.cpp`
test inclusion and symbol substitution need correct paths until stage 2 replaces them.

### Stage 2 — Make boundaries buildable and testable

Create the backend targets specified by the source plan. Move test registration
to `tests/CMakeLists.txt`, packaging to `cmake/Packaging.cmake`, and graph setup to
`cmake/TrafficGraph.cmake`. Make desktop dependencies optional for headless builds.
Keep UI target extraction incremental so native resource linking stays verifiable.
Add the Make command facade, native presets and local mihomo build/staging target.
Real-core tests and packages must consume this output; no fallback to downloaded
or unrelated installed binaries. Record source/toolchain/architecture provenance.

Replace proxy source inclusion with an injected command runner, browser symbol
replacement with injected operations, and internal QTimer mutation with a deadline
dependency. Add scoped settings/environment fixtures, loopback HTTP/WebSocket
fixtures and a portable fake-core executable. Split existing suites by responsibility
while retaining distinct regression assertions. Add dependency-graph, public-header
and headless build checks. Ordinary success paths receive the same attention as
failure cases. Gate existing features using F01–F10 rather than a raw test count.

### Stage 3 — Own services explicitly

Add application coordinators and a portable `MihomoBackend`. Main constructs and
owns services; pages receive their dependencies. Remove BackupStore discovery through
the widget tree. Preserve save/restore gates and shutdown ordering. UI, tray and
hotkeys use one routing service; they no longer mutate the backend independently.

Implement the component interfaces only for capabilities actually consumed. Keep
existing `CoreProcess`, `MihomoClient` and `ProviderClient` behind the adapter, with
temporary forwarding methods for incremental UI migration. The facade is asynchronous
and maintains request IDs/generations, explicit ownership and completion outcomes.
Add a fake implementation and shared backend contract tests. Introduce real-core
and app smoke tests now; extend them in subsequent stages.

### Stage 4 — Deliver reusable configuration

First extract composition without changing results. Then add versioned global and
per-profile presets and explicit merge/replace/remove/rule-order operations. Keep
legacy semantics in their adapter. Add provenance, validation diagnostics, preview
and last-good recovery. Protect controller-owned fields at the final boundary.

Network and managed-policy constraints get explicit extension points, but do not
pretend to implement those features yet. Configuration generation uses immutable
inputs; I/O, resource preparation and apply/recovery stay in application services.
Test valid representative subscriptions, normal edits, precedence, persistence,
invalid updates and concurrent refresh as complete contracts.

### Stage 5 — Add network modules with observable state

Start with Tailscale detection and diagnostics that do not change the machine.
Add plans for routes, DNS and proxy bypass only after observations are reliable.
The host validates combined requirements and owns changes/restoration. A plugin
contributes requirements, never independent uncoordinated system edits.

Validate IPv4/IPv6, actual subnet routes, private DNS, reconnect and sleep/wake.
Initially reject exit-node and overlapping-route combinations without validated
support. Keep platform claims tied to native evidence. This stage can proceed
independently of identity implementation once stage 4's interfaces are stable.

### Stages 6–7 — Add management as a separate service

Stage 6 delivers a thin end-to-end flow: browser login, enrollment, assigned policy,
desktop application, and applied-revision acknowledgement. Provision a test Casdoor
tenant and Feishu application when integration testing begins. Keep their secrets
in the server/test credential stores. Backend stack/deployment choices are decided
before implementing the API; their clients remain protocol-based.

Resolve organization membership server-side. Enforce mandatory settings/rules and
allowed actions in application services, including future COM/plugin entry points.
Keep standalone mode functional. Cover refresh/logout, tenant switching, revocation,
offline grace and policy expiry. Complete the assignment/admin workflow in stage 7.

Usage reporting uses supported cumulative counters and durable idempotent batches;
rate charts are not a billing source. Show gaps and exclude unobserved bypass traffic
from claims. Hard quotas or authoritative accounting require a separate controlled
gateway deliverable; the desktop plan does not provide tamper-proof enforcement.

### Stage 8 — Prove extensibility with a second integration

Consume an existing EasyConnect SOCKS/HTTP endpoint before adding container control.
Model Fudan settings as a preset whose current gateway/authentication/DNS behavior
is validated using a test account. Limit lifecycle control to owned labeled resources.

Once both Tailscale and EasyConnect exercise the contracts, add versioned external
process plugins, capability negotiation, bounded IPC, crash handling, and a minimal
independent provider example. Explicitly document that process separation is not
an OS security sandbox. Keep credentials and privileged operations behind host APIs.

### Stages 10–12 — Application capture add-ons

Implement the [capture plan](CAPTURE_MODULE_PLAN.md) as two modules sharing target
resolution, lifecycle, catalog/export and schedules. Monitoring uses native sensors
feeding a containerized metadata backend; HTTPS paths are unavailable without TLS
interception. MITM uses a supervised local-source mitmproxy worker, qualifying an
explicit app proxy first and native local targeting next. Incoming server connections
need distinct capture/reverse-proxy support from replies to outgoing requests.

Expose capability and completeness states in the UI. TLS trust, pinning, application
encryption, streaming limits and unsupported protocols prevent a universal 'all
content' guarantee. Capture-only forwarding preserves qualified application content,
not identical encrypted packets. Add native flow saving, bounded derived exports,
persisted periodic jobs and selected scripts with timeout/recovery. Shared network
coordination prevents loops with mihomo/TUN/Tailscale/EasyConnect.

## Exact homes for new code

These paths supplement the existing-file move table; `.*` means a header/source
pair where appropriate. Internal helper filenames can evolve within their owner.

| New responsibility | Destination |
| --- | --- |
| Core contracts and values | `src/core/backend/`; published portable component ABI in `src/core/component/` |
| Portable component loader | `src/integrations/component/` |
| Mihomo facade | `src/core/mihomo/mihomo_backend.*` |
| Runtime and routing coordinators | `src/app/runtime/runtime_coordinator.*`, `routing_controller.*` |
| Shutdown and backup coordination | `src/app/lifecycle/shutdown_coordinator.*`, `src/app/backup/backup_coordinator.*` |
| Pure composition and presets | `src/core/config/config_composer.*`, `preset_store.*` |
| Identity session coordination | `src/app/identity/session_manager.*` |
| OIDC and management transports | `src/integrations/identity/`, `src/integrations/management/` |
| Pure policy and network planning | `src/core/policy/`, `src/core/network/` |
| Network application/rollback | `src/app/network/network_coordinator.*` |
| Tailscale / EasyConnect | `src/integrations/network/tailscale/`, `easyconnect/` |
| Credential store adapters | `src/platform/credentials/` |
| Usage collection / delivery | `src/app/usage/`, `src/integrations/management/` |
| Registry / external plugin host | `src/extensions/` |
| Optional native Windows COM binding/server | `src/integrations/windows/com/`, `src/services/windows/mihomo_com/` (not baseline module path) |
| Management API and admin client | `management/api/`, `management/admin/` (separate build/deployment) |
| Architecture / backend contracts | `tests/architecture/`, `tests/contracts/` |
| Coordinators / complete workflows | `tests/app/`, `tests/workflows/` |
| Real display / package / performance | `tests/native/`, `tests/packaging/`, `tests/benchmarks/` |
| Local core build / Make facade | `cmake/Mihomo.cmake`, root `Makefile`, `CMakePresets.json`, `scripts/build/` |
| OS-specific distribution | `packaging/macos/`, `packaging/windows/`, `packaging/linux/` |
| Capture contracts/catalog/export | `src/core/capture/`, `src/core/capture/store/` |
| Capture targeting/coordination/scheduling | `src/app/capture/`, `src/app/capture/schedules/` |
| Monitor / mitmproxy adapters | `src/integrations/capture/monitor/`, `src/integrations/capture/mitmproxy/` |
| Native sensors/trust | `src/platform/capture/<os>/`, `src/platform/trust/<os>/` |
| Python worker bridge / monitor container | `src/services/capture/mitmproxy/`, repository-root `services/monitor/` |
| Capture UI / add-on packaging | `src/ui/pages/capture/`, `packaging/capture/` |

## Dependencies and completion

Execution foundation: **P0 → P1 → P2 → P3**, with three bounded worker packages per
wave. P4's configuration/UI work and portable component ABI qualification proceed against reviewed
contracts. P5's network, identity and policy tracks can advance independently until
cross-feature integration. P6/P7 successors consume their specific predecessor
artifacts, not unfinished sibling work; see the execution plan for finer dependencies.
Managed component operations require authority-side policy enforcement. External plugins wait for
both built-in network integrations. Standalone improvements can be released before
management/plugins, without claiming unfinished native gates have passed.

Useful milestones are: readable/testable project (stage 2), reusable standalone
client (stage 4), Tailscale support (stage 5), managed deployment (stage 7), and
extension SDK plus EasyConnect (stage 8). A native Windows COM bridge is optional
and would have its own separately scoped milestone.
Portable component qualification is part of the foundation. Application monitoring,
MITM inspection and scheduled capture add further independent product milestones.
The foundation/main cutover additionally requires all G1–G6 acceptance for the
claimed target matrix, fresh current documentation and verified legacy-v1 preservation.

Every stage includes implementation, tests, documented behavior and migration notes.
Run applicable existing suites as well as new contracts. Preserve narrow rollback
points; do not revert a stage by resetting unrelated work. Report platform checks
that were unavailable. Estimate calendar dates after the baseline/build and Windows
COM spike expose environment and integration costs; no date is implied by stage order.
