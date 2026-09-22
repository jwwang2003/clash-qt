# Test suite index

Current P4 registration: **59 CTest entries**, measured from
`ctest --show-only=json-v1` on macOS arm64. Counts are measurements, not a
platform-support claim. Final qualification results are recorded separately from
registration; a registered test is not evidence that it passed.

## Commands and lanes

Use a build directory you own:

```sh
make test BUILD_DIR=build/p4
make test-integration BUILD_DIR=build/p4
make package BUILD_DIR=build/p4
```

- `make test`: **54 entries**, excluding `native|privileged|benchmark|real-core`.
- `make test-integration`: **20 entries**, selecting `real-core|integration` and
  building the pinned local engine first.
- `native|privileged`: **zero registered entries**. The reserved native lane is
  not evidence; helper fixture tests carry `service` and remain in routine tests.
- `benchmark`: two optional native graphics entries and the headless connection
  marshalling measurement. The normal lane separately tests codec correctness.

Each entry receives its own `CLASH_QT_DATA_DIR`; GUI entries select offscreen and
software rendering where registered. Test subprocesses preserve explicit data
isolation. App smoke additionally supplies its own helper socket and controller
fixtures; a data directory alone is not permission to query an installed helper.
Tests never install/remove/restart the helper or change OS proxy/VPN/trust state.

Feature IDs: F01 profiles; F02 subscriptions; F03 configuration; F04 lifecycle;
F05 routing; F06 proxies/providers/rules; F07 telemetry; F08 backup/restore;
F09 preferences/integrations; F10 application/distribution.

## Registered entries

| CTest name | Labels | Feature contracts |
| --- | --- | --- |
| `controller` | core, integration, mihomo | F04, F05, F06 |
| `provider` | core, integration, mihomo | F02, F06 |
| `backup` | backups, core, integration | F08 |
| `traffic-history` | core, telemetry, unit | F07 |
| `privileged-service` | integration, platform, service | F05, F09 |
| `system-proxy-async` | platform, proxy, unit | F05 |
| `proxy` | platform, proxy, unit | F05 |
| `tray` | shell, ui | F05, F06, F10 |
| `routing-controls` | shell, ui | F05 |
| `service-settings` | pages, service, ui | F05, F09 |
| `dashboard-async` | shell, ui | F09, F10 |
| `privileged-helper` | platform, service, unit | F05, F09 |
| `privileged-helper-ipc` | platform, service, unit | F05, F09 |
| `preferences` | core, preferences, unit | F09 |
| `scoped-environment` | support, unit | — |
| `loopback-server` | support, unit | — |
| `fake-core` | support, unit | — |
| `backend-contract` | backend, contract, unit | F04, F05, F06, F07 |
| `backend-bridge` | backend, contract, unit | F04, F05, F06, F07 |
| `component-contract` | component, contract, unit | F10 |
| `config-generation` | config, core, integration | F01, F03, F05 |
| `profile-store` | core, integration, profiles | F01, F02, F03 |
| `core-process` | core, integration, mihomo | F04, F09 |
| `runtime-maintenance` | core, integration, profiles | F01, F08 |
| `home-page` | pages, ui | F07, F10 |
| `traffic-graph` | pages, ui | F07 |
| `proxies-page` | pages, ui | F06 |
| `connections-page` | pages, ui | F07 |
| `providers-page` | pages, ui | F06 |
| `rules-page` | pages, ui | F06 |
| `logs-page` | pages, ui | F07 |
| `traffic-graph-frames` | benchmark, ui | F07 |
| `traffic-frame-pacing` | benchmark, ui | — |
| `preset-editor` | config, ui | F01, F03 |
| `shutdown-coordinator` | app, lifecycle, unit | F04, F05, F10 |
| `backup-coordinator` | app, backup, unit | F01, F03, F08 |
| `runtime-coordinator` | app, runtime, unit | F01, F03, F04 |
| `routing-controller` | app, runtime, unit | F05 |
| `backend-real-contract` | contract, core, integration, mihomo | F04, F05, F06, F07 |
| `engine-discovery` | core, mihomo, unit | F04, F10 |
| `backend-real-core` | core, integration, mihomo, real-core | F04, F05 |
| `w01-first-launch` | app, integration, workflow | F01, F03, F04, F10 |
| `w02-subscription-update` | app, config, integration, workflow | F01, F02, F03, F04 |
| `w04-restore` | app, integration, workflow | F01, F02, F03, F08, F09 |
| `w05-recovery` | app, integration, workflow | F01, F04, F10 |
| `w03-routing-controls` | app, integration, ui, workflow | F04, F05, F10 |
| `app-smoke` | app, integration, packaging, workflow | F04, F09, F10 |
| `w02-real-core` | app, config, integration, real-core, workflow | F01, F02, F03, F04 |
| `arch-graph` | architecture | — |
| `arch-public-headers` | architecture | — |
| `arch-selftest` | architecture | — |
| `config-composer` | config, core, unit | F03 |
| `preset-document` | config, core, unit | F03 |
| `chain-snapshot` | config, core, unit | F03 |
| `preset-store` | config, core, integration | F01, F03 |
| `effective-config-preview` | config, core, integration | F01, F03 |
| `abi-contract` | component, contract, integration | F04, F05, F06, F07, F10 |
| `connection-snapshot-codec` | component, unit | F07, F10 |
| `connection-snapshot-marshal` | benchmark, component | F07 |

## Shared backend and binary boundary

`backend-real-contract-tests` retains its implementation-specialist cases and
runs **one shared assertion set** over direct fake, direct real, module fake and
module real fixtures. Required missing/unloadable modules fail; they do not skip.
The shared set covers identity, request/generation stamping, observer admission
and removal, non-re-entrancy, ownership, validation preservation, provider
coalescing, TUN read-back, privileged status, same-address session replacement and
lifecycle invalidation. The shipping module has no test-injection commands.

The older `backend-contract` and specialist cases retain virtual-clock and
readiness/termination failure coverage that the shared facade cannot inject.
`backend-real-core` and `w02-real-core` supply actual-engine evidence; a test using
the compiled fixture core is not evidence about mihomo.

`abi-contract` checks the actual shared artifacts: factory negotiation, identity,
reference transfer, retained buffers/errors and roots, close/drain/unload,
owner-thread cleanup with any-thread refcount calls, malformed packets,
production-sequence observer admission, reverse service injection, and timestamp
representation. The host Qt facade and bridge keep their existing signatures.
The component remains a supervisor of a child process; proxy payloads do not
cross this boundary.

The separate [sample consumer](../examples/component_consumer/README.md) has its
own CMake project. Qualification builds it against the installed public header
set, without linking the desktop implementation, then drives the pinned engine.
Qualification requires actual unmapping, not just a successful `dlclose` return.

## Configuration and UI

- `config-composer`: legacy-equivalent precedence, explicit merge/replace/remove
  and rule ordering, false/empty values, protected fields, provenance and
  diagnosed depth refusal. The composer has no filesystem/network/process work.
- `preset-document`: version/schema/path/operation validation.
- `chain-snapshot`: immutable legacy merge/script contents and preserved fallback.
- `preset-store`: persistence, rejected-save preservation and last-good recovery
  for malformed, missing and unreadable primary documents.
- `effective-config-preview`: no runtime files or launch side effects; only the
  latest immutable request is delivered; failed composition preserves runtime.
- `preset-editor`: real ProfileStore/composer wiring, scopes, edits, validation,
  ordering and displayed provenance/diagnostics. Failure keeps the prior preview
  explicitly marked as older.
- `backup`: preset roundtrip, pre-preset archive defaults and validation before
  any restore writes, alongside the existing archive contracts.

Composition acceptance is not engine acceptance. The backend separately validates
candidates with the selected engine while the applied core remains alive.

## Journeys

| Journey | Entries | Evidence scope |
| --- | --- | --- |
| W01 first launch | `w01-first-launch` | Import, start, readiness, stop, quit and reopen through the module |
| W02 subscription update | `w02-subscription-update`, `w02-real-core` | Local HTTP subscription, presets/overrides, refresh, rejection, stale URL reply, persistence; pinned real-engine smoke |
| W03 routing | `w03-routing-controls` | Real shell surfaces and confirmed state; substituted OS commands |
| W04 restore | `w04-restore` | Restore with services and a held subscription reply in flight |
| W05 recovery | `w05-recovery` | Crash/reconnect/restart and confirmed bounded shutdown; refusing child must announce its refusal before timing |
| Executable smoke | `app-smoke` | Shipped binary, module selection, isolated helper/controller routing, single instance, startup wiring and data-directory precedence |

Journeys using generated configs are `RUN_SERIAL` because their controller uses
port 29097. Do not run separate journey processes against that port concurrently.
Port-unavailable skips are unavailable evidence, not passes. W02's two CTest
entries select their respective function sets so the other lane is not counted
as a skip. The real-core lane requires the build's executable and matching
provenance/hash; missing artifacts fail.

Smoke uses local `QLocalServer`/HTTP fixtures and a deliberately oversized service
configuration. The size guard covers core start only; explicit socket isolation
is also required for the Settings page's startup status request. Generic isolated
launches cannot fall back to another client's controller or geo-data directory.

## Benchmarks and platform limits

`connection-snapshot-codec` selects the three correctness slots from the same
binary used by `connection-snapshot-marshal`. The benchmark entry selects only
measurements, including packed versus naive encoding. Record build mode, workload,
time and payload size. D8 requires a measured cost; it does not promise a speedup
on every build or machine.

`traffic-graph-frames` requires `CLASH_QT_VERIFY_GRAPH_FRAMES=1` and a native display;
`traffic-frame-pacing` requires `CLASH_QT_MEASURE_FRAMES=1`. They are excluded from
routine qualification and do not constitute native-rendering evidence when skipped.

Current qualification target is macOS arm64. Windows and Linux have **zero**
compile/run evidence; conditional code and registered tests do not qualify them.
Some older core-process fixtures remain POSIX-specific. No signed-release,
notarization, native-GPU or installed privileged-service claim is made here.

## Architecture and proof

The evaluated CMake graph, standalone public-header consumers and architecture
checker self-tests cover dependency direction and its ratchet. Both P4 G2
migration exceptions are removed. The permanent allowance for the six tests whose
subject is the private implementation stays; it does not permit application links.

High-value behavior assertions were checked by external-copy inversions. A
surviving mutation is not called coverage; redundant checks and unmeasured limits
are recorded explicitly in qualification evidence. Fresh-clone Make/package runs
and independent read-only audits close the integration gate, not a green unit
count alone. Historical suite-partition mappings remain retrievable in Git.
