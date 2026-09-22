# P4 dispatch and ownership

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
