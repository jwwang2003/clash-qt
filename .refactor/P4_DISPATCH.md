# P4 dispatch and ownership

Current active source leases: NONE. All implementation workers returned STABLE.
Independent config and ABI subgate rechecks returned GO. Coordinator is at the
source barrier and beginning fresh-clone qualification of the combined result.
Status/evidence scratch: `/tmp/clash-qt-p4.w0Y8Uo`, `status.py` reads worker logs.
The rounds below are historical dispatch records; later entries supersede status.

Coordinator: contracts, architecture decisions, all CMake/Make/preset/build files,
main.cpp, tests/architecture/architecture.json, ledger, integration and git.
Workers: Opus explicitly selected on every dispatch; no nested agents.
No source builds until a stable-source window or an external snapshot.

## Round 1 (active, read-only)

- COMPONENT-ABI analysis: ABI/module/shim, lifetime, injection, contract parity.
- CFG-CORE analysis: pure composition, presets, provenance, diagnostics/recovery.
- CFG-UI/W02 analysis: UI wiring and subscription journey.

Writable paths: none. Reports under 350 words.

## Round 2 implementation (active)

All workers explicitly Opus. First analysis reports resolved to claude-opus-5,
zero permission denials. Design frozen in P4_ABI_CONTRACT/P4_CONFIG_CONTRACT.

- ABI: new core/component/abi/**, core/mihomo/module/**,
  integrations/component/**, tests/contracts/component/abi/**,
  tests/support/component/**, benchmarks/connection_snapshot_marshal_test.cpp,
  examples/component_consumer/**. No build files.
- CFG: core/config/**, core/profiles/** (no build files), tests/core/config/**,
  tests/core/profiles/**, tests/core/{config_generation,profile_store,maintenance}_test.cpp.
- UI: profiles_page.{h,cpp}, new profiles/{preset_editor,effective_config_view}.{h,cpp},
  tests/ui/preset_editor_test.cpp.

No repo-source builds in this round. Workers may compile external snapshots and
prove inversions there. No git mutations by workers.

## Integration finding queued for next lease

The producer/consumer sweep found BackupStore's explicit roots/allowedPath lists
name runtime-overrides.json but not the new presets.json or presets.last-good.json.
Preset state would disappear across backup/restore without a matching consumer
change. Assign backup store + backup contract tests to CFG worker after its current
lease stabilizes; preserve old archives by supplying a version-1 empty document.

## UI lease stable; W02 lease active

CFG-UI returned STABLE, Opus 5, real-store external test and inversion evidence
in scratch ui/registrations.md. No new source writes on that lease.
Next Opus worker W02 owns only workflow_support.h, w01_first_launch_test.cpp,
w05_recovery_test.cpp, new w02_subscription_update_test.cpp, app_smoke_test.cpp,
and tests/support/loopback_server.{h,cpp}, loopback_server_test.cpp. It migrates
journeys through the module and adds fixture/pinned-core subscription coverage.
Build registrations stay coordinator-owned.

## CFG lease stable; backup integration active

CFG-CORE returned STABLE with eight external-snapshot suites and twelve detected
inversions. Its final wiring-gap report was stale: presetsChanged is already
connected in main.cpp, and four of five suites were registered while it worked.
Coordinator adds chain-snapshot as fifth. Evidence is checked against source/logs.
Next Opus worker owns only src/core/backups/backup_store.cpp and
 tests/core/backup_test.cpp for preset backup/restore compatibility and proofs.

## Backup lease stable; shared contract lease active

Backup integration returned STABLE with roundtrip/legacy/invalid-state cases and
five external inversions detected. Coordinator adds its pure-composer link edges.
Next Opus worker owns tests/core/mihomo/backend_real_contract_test.cpp,
new tests/contracts/backend/common/** and
new tests/support/backend/common_contract_driver.{h,cpp}. Shared assertion/data
rows run fake, direct real, module real and module fake through the existing
implementation-subject target; the permanent G2 allowance is not expanded.

## Initial ABI lease stable; hardening lease active

Initial ABI worker returned STABLE with real engine start/stop evidence and an
actual dlclose crash. Its permanent activated-image mapping workaround is NOT
accepted as gate closure; see P4_ABI_REVIEW.md. Follow-up Opus worker owns the
same ABI paths (excluding build files), plus private mihomo_backend.{h,cpp},
process/core_process.{h,cpp}, tests/support/backend/fake_backend.{h,cpp} only as
needed for dispatch sequencing and owned asynchronous work. Shared-contract
worker owns separate test/driver files; W02 owns separate workflow/fixture files.
No combined repo build until all are stable. No public Qt facade/bridge changes.

## W02 safety restart

Prior W02 worker interrupted and exited; replacement Opus worker continues the
same lease plus tests/workflows/composition_root_audit.h. It must rebuild fixed
main.cpp before any app run and always set isolated CLASH_QT_SERVICE_SOCKET and
CLASH_QT_DATA_DIR on children. Prove routing using a local helper fixture. Invert
toward a different isolated socket, NEVER toward the installed default helper.

## Common and W02 stable; core corrections and config audit active

Common worker delivered 15 shared cases across four implementations, but with
explicit expected-failure markers for real backend at-rest attachment and module
observer admission. Those are OPEN, not green acceptance. W02 safety follow-up
is stable with corrected local helper routing and real-core evidence; it also
found stale replies from an identical-address retired engine clearing the new
connection. Core-parity worker now owns only mihomo_client.{h,cpp},
tests/core/controller_test.cpp, backend_real_contract_test.cpp, new common case
and common driver files. It removes expected failures after actual fixes and
makes missing required module artifacts fail. It waits for ABI_STABLE before
verifying the new binary protocol; no concurrent source ownership with ABI worker.

A separate Opus read-only auditor owns scratch only and audits stable configuration,
UI and backup. It can report GO only for that subgate, not all of P4. Active slots:
ABI hardening, core parity correction, independent config subgate audit.

## ABI hardening stable; independent ABI audit active

ABI hardening returned STABLE (wire revision 2, real activated image demonstrably
unmapped; shared runtime held for Qt cleanup). ABI_STABLE scratch marker released
core parity worker to sync and verify final protocol. A new Opus read-only ABI
reader audits an independent copy of that stable snapshot, including retained-root
retry, final-release thread affinity and date-time representation. It owns scratch
only. Core client/common test files are still exclusively the core-parity worker's.
Coordinator is removing an unsupported universal speed assertion from the benchmark:
D8 asks for measurement, not an assumption that packing is faster in every build.

## Config audit findings require correction despite its narrow GO

Read-only config audit demonstrated F1 unreadable-primary silent loss and F2
successful-but-truncated deep merge. Coordinator keeps those product gates OPEN.
Config-fix Opus worker owns only config_composer.{h,cpp}, profile_store.{h,cpp},
config_composer_test.cpp, preset_document_test.cpp, preset_store_test.cpp and
effective_config_preview_test.cpp. It also makes the pure composer honor trusted
mixed-port overrides directly (F3); cosmetic YAML order is not a blocker.
Auditor remains repository read-only and finished. Active slots: ABI read-only
audit, core parity correction, config audit fixes.

## Core parity stable; isolated controller discovery lease active

Core parity returned STABLE: 95 backend contract functions/rows, no skips or
expected failures; 21 controller functions, seven detected inversions. Coordinator
committed it as 39a5c45. A bounded Opus worker now owns controller_discovery.{h,cpp},
mihomo_client.cpp (only invalid-stream opening/retained intent), engine_discovery_test.cpp,
app_smoke_test.cpp and composition_root_audit.h. It prevents isolated app launches
from implicitly attaching to another installed client, preserving explicit fixture
endpoints and later managed telemetry. Main.cpp remains coordinator-owned and now
checks discovered endpoint validity before attach. Active slots: config fixes,
read-only ABI audit, isolated controller discovery.

## Independent ABI audit NO-GO; final ABI repair lease active

Read-only auditor reproduced cross-thread final-Release crash, retained-root
unload retry failure/leak, and changed QDateTime representation. Final ABI repair
Opus worker owns ABI/module/shim/adjacent ABI tests/example/benchmark (no builds),
plus private CoreProcess and MihomoBackend only if pending async-work ownership
requires it. Isolation worker owns MihomoClient/controller discovery separately.
Config-fix owns composer/ProfileStore. No shared writes. Parent review specifies
owner-thread deferred cleanup with counted lifetime, recoverable root/lifetime
unload protocol, and representation-preserving timestamps with wire revision bump.

## Config fixes stable; independent recheck active

Config-fix returned STABLE with F1/F2/F3 changes, ten external suites and twelve
inversions. A fresh Opus read-only reader now tests those corrections and retained
legacy/UI/backup semantics in its own copy. It treats a deliberately replacing
preset document as replacement; recovery must preserve state when an edit starts
from the recovered document. Active slots: final ABI repair, isolated controller
discovery, independent config recheck. No config writer remains active.

## Isolation stable; targeted transport read-only audit active

Isolation worker returned STABLE (f04647f), with guarded controller discovery and
retained stream intent; its new executable smoke cases await final binary build.
A read-only Opus worker now probes a concrete remaining producer/consumer concern:
ProviderClient listens only to endpointChanged while same-address replacement
emits invalidating; test cross-generation coalescing/supersession and retired stream
callbacks. It uses a HEAD archive/private backend copy, never active ABI source.
Active slots: final ABI repair, independent config recheck, transport audit.

## Final ABI patch stable; final targeted fixes/recheck

ABI final repair returned STABLE with wire 3/timezone-preserving snapshots,
recoverable root unload and counted owner-thread cleanup/native tasks. Coordinator
requires one tightening: Release returning zero must preserve component-r1's actual
object-destruction meaning. RC worker owns only backend_session.{h,cpp}, ABI header
comments and abi_contract_test.cpp. Transport-fix worker owns client/provider/backend
and FakeBackend plus their/shared tests, disjoint from RC files. It addresses the
read-only transport audit's confirmed same-session/cross-generation defects.
A fresh read-only ABI reader tests a copy of the stable patch and waits for RC_STABLE
to recheck the return-value guarantee. Its other findings remain independent.
Active slots: transport fix, RC fix, read-only ABI recheck. No repo-wide build.

## New measured W05 deadline defect

Transport's broader snapshot run failed W05: refusing child ended at 2962 ms against
published 3000 ms. Timer-fix Opus worker owns only CoreProcess.cpp, core_process_test.cpp
and w05_recovery_test.cpp. It preserves native-work ownership, enforces no early
kill and gates on the fixture's refusal marker. No tolerance weakening. Other
workers own no overlapping file. Active: transport fix, read-only ABI recheck,
termination deadline fix. Full repo build still waits for the source barrier.
