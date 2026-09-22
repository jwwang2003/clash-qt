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
