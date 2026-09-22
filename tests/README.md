# Test suite index

Current registration: **59 CTest entries**, measured from
`ctest --show-only=json-v1` on macOS arm64. Counts are measurements, not a
platform-support claim. Final qualification results are recorded separately from
registration; a registered test is not evidence that it passed.

## Commands and lanes

Use a build directory you own:

```sh
make test BUILD_DIR=build/local
make test-integration BUILD_DIR=build/local
make package BUILD_DIR=build/local
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

The "Feature contracts" column names the product areas an entry is evidence
for: `profiles`, `subscriptions`, `configuration`, `lifecycle`, `routing`,
`proxies-providers-rules`, `telemetry`, `backup-restore`,
`preferences-integrations` and `application-distribution`.

## Registered entries

| CTest name | Labels | Feature contracts |
| --- | --- | --- |
| `controller` | core, integration, mihomo | lifecycle, routing, proxies-providers-rules |
| `provider` | core, integration, mihomo | subscriptions, proxies-providers-rules |
| `backup` | backups, core, integration | backup-restore |
| `traffic-history` | core, telemetry, unit | telemetry |
| `privileged-service` | integration, platform, service | routing, preferences-integrations |
| `system-proxy-async` | platform, proxy, unit | routing |
| `proxy` | platform, proxy, unit | routing |
| `tray` | shell, ui | routing, proxies-providers-rules, application-distribution |
| `routing-controls` | shell, ui | routing |
| `service-settings` | pages, service, ui | routing, preferences-integrations |
| `dashboard-async` | shell, ui | preferences-integrations, application-distribution |
| `privileged-helper` | platform, service, unit | routing, preferences-integrations |
| `privileged-helper-ipc` | platform, service, unit | routing, preferences-integrations |
| `preferences` | core, preferences, unit | preferences-integrations |
| `scoped-environment` | support, unit | — |
| `loopback-server` | support, unit | — |
| `fake-core` | support, unit | — |
| `backend-contract` | backend, contract, unit | lifecycle, routing, proxies-providers-rules, telemetry |
| `backend-bridge` | backend, contract, unit | lifecycle, routing, proxies-providers-rules, telemetry |
| `component-contract` | component, contract, unit | application-distribution |
| `config-generation` | config, core, integration | profiles, configuration, routing |
| `profile-store` | core, integration, profiles | profiles, subscriptions, configuration |
| `core-process` | core, integration, mihomo | lifecycle, preferences-integrations |
| `runtime-maintenance` | core, integration, profiles | profiles, backup-restore |
| `home-page` | pages, ui | telemetry, application-distribution |
| `traffic-graph` | pages, ui | telemetry |
| `proxies-page` | pages, ui | proxies-providers-rules |
| `connections-page` | pages, ui | telemetry |
| `providers-page` | pages, ui | proxies-providers-rules |
| `rules-page` | pages, ui | proxies-providers-rules |
| `logs-page` | pages, ui | telemetry |
| `traffic-graph-frames` | benchmark, ui | telemetry |
| `traffic-frame-pacing` | benchmark, ui | — |
| `preset-editor` | config, ui | profiles, configuration |
| `shutdown-coordinator` | app, lifecycle, unit | lifecycle, routing, application-distribution |
| `backup-coordinator` | app, backup, unit | profiles, configuration, backup-restore |
| `runtime-coordinator` | app, runtime, unit | profiles, configuration, lifecycle |
| `routing-controller` | app, runtime, unit | routing |
| `backend-real-contract` | contract, core, integration, mihomo | lifecycle, routing, proxies-providers-rules, telemetry |
| `engine-discovery` | core, mihomo, unit | lifecycle, application-distribution |
| `backend-real-core` | core, integration, mihomo, real-core | lifecycle, routing |
| `first-launch` | app, integration, workflow | profiles, configuration, lifecycle, application-distribution |
| `subscription-update` | app, config, integration, workflow | profiles, subscriptions, configuration, lifecycle |
| `restore` | app, integration, workflow | profiles, subscriptions, configuration, backup-restore, preferences-integrations |
| `recovery` | app, integration, workflow | profiles, lifecycle, application-distribution |
| `routing-controls-journey` | app, integration, ui, workflow | lifecycle, routing, application-distribution |
| `app-smoke` | app, integration, packaging, workflow | lifecycle, preferences-integrations, application-distribution |
| `subscription-update-real-core` | app, config, integration, real-core, workflow | profiles, subscriptions, configuration, lifecycle |
| `arch-graph` | architecture | — |
| `arch-public-headers` | architecture | — |
| `arch-selftest` | architecture | — |
| `config-composer` | config, core, unit | configuration |
| `preset-document` | config, core, unit | configuration |
| `chain-snapshot` | config, core, unit | configuration |
| `preset-store` | config, core, integration | profiles, configuration |
| `effective-config-preview` | config, core, integration | profiles, configuration |
| `abi-contract` | component, contract, integration | lifecycle, routing, proxies-providers-rules, telemetry, application-distribution |
| `connection-snapshot-codec` | component, unit | telemetry, application-distribution |
| `connection-snapshot-marshal` | benchmark, component | telemetry |

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
`backend-real-core` and `subscription-update-real-core` supply actual-engine
evidence; a test using the compiled fixture core is not evidence about mihomo.

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
| First launch | `first-launch` | Import, start, readiness, stop, quit and reopen through the module |
| Subscription update | `subscription-update`, `subscription-update-real-core` | Local HTTP subscription, presets/overrides, refresh, rejection, stale URL reply, persistence; pinned real-engine smoke |
| Routing | `routing-controls-journey` | Real shell surfaces and confirmed state; substituted OS commands |
| Restore | `restore` | Restore with services and a held subscription reply in flight |
| Recovery | `recovery` | Crash/reconnect/restart and confirmed bounded shutdown; refusing child must announce its refusal before timing |
| Executable smoke | `app-smoke` | Shipped binary, module selection, isolated helper/controller routing, single instance, startup wiring and data-directory precedence |

Journeys using generated configs are `RUN_SERIAL` because their controller uses
port 29097. Do not run separate journey processes against that port concurrently.
Port-unavailable skips are unavailable evidence, not passes. The subscription
journey's two CTest entries select their respective function sets so the other
lane is not counted as a skip. The real-core lane requires the build's executable
and matching provenance/hash; missing artifacts fail.

A journey's executable target still carries its source file's stem --
`first_launch_test.cpp` builds `first-launch-tests` -- because the
architecture check declares CMake target names. The registered name in the table
above is what selects the test.

Smoke uses local `QLocalServer`/HTTP fixtures and a deliberately oversized service
configuration. The size guard covers core start only; explicit socket isolation
is also required for the Settings page's startup status request. Generic isolated
launches cannot fall back to another client's controller or geo-data directory.

## Benchmarks and platform limits

`connection-snapshot-codec` selects the three correctness slots from the same
binary used by `connection-snapshot-marshal`. The benchmark entry selects only
measurements, including packed versus naive encoding. Record build mode, workload,
time and payload size. The packed layout was adopted against a measured cost,
not against a promised speedup: the ratio differs by build mode and machine.

`traffic-graph-frames` requires `CLASH_QT_VERIFY_GRAPH_FRAMES=1` and a native display;
`traffic-frame-pacing` requires `CLASH_QT_MEASURE_FRAMES=1`. They are excluded from
routine qualification and do not constitute native-rendering evidence when skipped.

Current qualification target is macOS arm64. Windows and Linux have **zero**
compile/run evidence; conditional code and registered tests do not qualify them.
Some older core-process fixtures remain POSIX-specific. No signed-release,
notarization, native-GPU or installed privileged-service claim is made here.

## Architecture and proof

The evaluated CMake graph, standalone public-header consumers and architecture
checker self-tests cover dependency direction and its ratchet. Both migration
exceptions that let application code reach the component-private engine
implementation are removed. The permanent allowance for the six tests whose
subject is that private implementation stays; it does not permit application
links.

High-value behavior assertions were checked by external-copy inversions. A
surviving mutation is not called coverage; redundant checks and unmeasured limits
are recorded explicitly in qualification evidence. Fresh-clone Make/package runs
and independent read-only audits close the integration gate, not a green unit
count alone. Historical suite-partition mappings remain retrievable in Git.
