# Test suite index

The coverage index `docs/TEST_STRATEGY.md` ("Feature contracts") and
`docs/IMPLEMENTATION_ROADMAP.md` require: every registered suite, the coverage
IDs it carries, the environment it needs, and the gaps that remain.

It had drifted. It was written as the record of one job — partitioning the two
shared monoliths `tests/core/runtime_test.cpp` and `tests/ui/data_pages_test.cpp`
— and it still is that record, in the second half. But it listed 13 of the 42
tests CTest registers, and it carried no workflow IDs at all, so ten suites
built after it (the application-coordinator, backend-contract and real-engine
lanes) existed nowhere in it. The registered-suite table below is now the index;
everything after it is detail.

Feature identifiers are the ones used throughout the refactor plan:

| ID | Feature |
| --- | --- |
| F01 | Profiles |
| F02 | Subscriptions |
| F03 | Configuration |
| F04 | Core lifecycle |
| F05 | Routing |
| F06 | Proxies / providers / rules |
| F07 | Telemetry / logs / connections |
| F08 | Backup / restore |
| F09 | Preferences / integrations |
| F10 | Application / distribution |

Workflow identifiers `W01`–`W05` are the five complete journeys in
`docs/TEST_STRATEGY.md` ("Complete workflows"). They are tracked separately
below, because no suite currently runs one end to end.

---

## Registered suites

Every name CTest registers, in lane order. `make test` runs all of them except
the `native`, `privileged`, `benchmark` and `real-core` labels.

### Core lane

| CTest name | Target | Source | Labels | Features |
| --- | --- | --- | --- | --- |
| `controller` | `controller-tests` | `core/controller_test.cpp` | core, integration, mihomo | F04, F05, F06 |
| `provider` | `provider-tests` | `core/provider_test.cpp` | core, integration, mihomo | F06, F02 |
| `backup` | `backup-tests` | `core/backup_test.cpp` | backups, core, integration | F08 |
| `traffic-history` | `traffic-history-tests` | `core/traffic_history_test.cpp` | core, telemetry, unit | F07 |
| `preferences` | `preferences-tests` | `core/preferences_test.cpp` | core, preferences, unit | F09 |
| `config-generation` | `config-generation-tests` | `core/config_generation_test.cpp` | config, core, integration | F03, F05, F01 |
| `profile-store` | `profile-store-tests` | `core/profile_store_test.cpp` | core, integration, profiles | F01, F02, F03 |
| `core-process` | `core-process-tests` | `core/core_process_test.cpp` | core, integration, mihomo | F04, F09 |
| `runtime-maintenance` | `runtime-maintenance-tests` | `core/maintenance_test.cpp` | core, integration, profiles | F08, F01 |
| `engine-discovery` | `engine-discovery-tests` | `core/mihomo/engine_discovery_test.cpp` | core, mihomo, unit | F04, F10 |
| `backend-real-contract` | `backend-real-contract-tests` | `core/mihomo/backend_real_contract_test.cpp` | contract, core, integration, mihomo | F04, F05, F06, F07 |
| `backend-real-core` | `backend-real-core-tests` | `core/mihomo/backend_real_core_test.cpp` | core, integration, mihomo, **real-core** | F04, F05 |

### Contract lane

| CTest name | Target | Source | Labels | Features |
| --- | --- | --- | --- | --- |
| `backend-contract` | `backend-contract-tests` | `contracts/backend/backend_contract_test.cpp` | backend, contract, unit | F04, F05, F06, F07 |
| `backend-bridge` | `backend-bridge-tests` | `contracts/backend/bridge/backend_bridge_test.cpp` | backend, contract, unit | F04, F05, F06, F07 |
| `component-contract` | `component-contract-tests` | `contracts/component/component_contract_test.cpp` | component, contract, unit | F10 |

`backend-contract` and `backend-real-contract` run the same backend-r3 semantics
against the fake and against the real `MihomoBackend`. A passing fake proves the
contract is implementable; only the real one is evidence about the engine, and
only `backend-real-core` is evidence about mihomo itself.

### Application lane

| CTest name | Target | Source | Labels | Features |
| --- | --- | --- | --- | --- |
| `shutdown-coordinator` | `shutdown-coordinator-tests` | `app/lifecycle/shutdown_coordinator_test.cpp` | app, lifecycle, unit | F04, F05, F10 |
| `backup-coordinator` | `backup-coordinator-tests` | `app/backup/backup_coordinator_test.cpp` | app, backup, unit | F08, F01, F03 |
| `runtime-coordinator` | `runtime-coordinator-tests` | `app/runtime/runtime_coordinator_test.cpp` | app, runtime, unit | F03, F04, F01 |
| `routing-controller` | `routing-controller-tests` | `app/runtime/routing_controller_test.cpp` | app, runtime, unit | F05 |

### Platform lane

| CTest name | Target | Source | Labels | Features |
| --- | --- | --- | --- | --- |
| `proxy` | `proxy-tests` | `platform/system_proxy_test.cpp` | platform, proxy, unit | F05 |
| `system-proxy-async` | `system-proxy-async-tests` | `platform/system_proxy_async_test.cpp` | platform, proxy, unit | F05 |
| `privileged-service` | `privileged-service-tests` | `platform/privileged_service_client_test.cpp` | integration, platform, service | F05, F09 |
| `privileged-helper` | `clash-qt-service-helper` | in-binary, run as `--self-test` | platform, service, unit | F09, F05 |
| `privileged-helper-ipc` | `privileged-helper-ipc-tests` | `src/services/macos/macos_helper.mm` (built with `CLASH_QT_HELPER_TESTING`, run as `--ipc-self-test`) | platform, service, unit | F05, F09 |

### UI lane

| CTest name | Target | Source | Labels | Features |
| --- | --- | --- | --- | --- |
| `tray` | `tray-tests` | `ui/tray_test.cpp` | shell, ui | F05, F06, F10 |
| `routing-controls` | `routing-controls-tests` | `ui/routing_controls_test.cpp` | shell, ui | F05 |
| `dashboard-async` | `dashboard-async-tests` | `ui/dashboard_async_test.cpp` | shell, ui | F09, F10 |
| `home-page` | `home-page-tests` | `ui/home_page_test.cpp` | pages, ui | F07, F10 |
| `traffic-graph` | `traffic-graph-tests` | `ui/traffic_graph_test.cpp` | pages, ui | F07 |
| `proxies-page` | `proxies-page-tests` | `ui/proxies_page_test.cpp` | pages, ui | F06 |
| `connections-page` | `connections-page-tests` | `ui/connections_page_test.cpp` | pages, ui | F07 |
| `providers-page` | `providers-page-tests` | `ui/providers_page_test.cpp` | pages, ui | F06 |
| `rules-page` | `rules-page-tests` | `ui/rules_page_test.cpp` | pages, ui | F06 |
| `logs-page` | `logs-page-tests` | `ui/logs_page_test.cpp` | pages, ui | F07 |

### Benchmark lane (excluded from `make test`)

| CTest name | Target | Source | Labels | Features |
| --- | --- | --- | --- | --- |
| `traffic-graph-frames` | `traffic-graph-frames-tests` | `benchmarks/traffic_graph_frames_test.cpp` | benchmark, ui | F07 |
| `traffic-frame-pacing` | `traffic-frame-pacing-tests` | `benchmarks/traffic_frame_pacing_test.cpp` | benchmark, ui | F07 |

### Fixture self-tests

The shared fixtures are themselves tested, because a fixture that lies makes
every suite that uses it lie with it. They carry no feature ID.

| CTest name | Target | Source | Labels |
| --- | --- | --- | --- |
| `scoped-environment` | `scoped-environment-tests` | `support/scoped_environment_test.cpp` | support, unit |
| `loopback-server` | `loopback-server-tests` | `support/loopback_server_test.cpp` | support, unit |
| `fake-core` | `fake-core-tests` | `support/fake_core_test.cpp` | support, unit |

### Architecture checks

Not feature coverage. They check the shape of the build, per
`docs/TEST_STRATEGY.md` "Architecture checks".

| CTest name | What it checks |
| --- | --- |
| `arch-graph` | The evaluated CMake target graph, read through the File API, against `architecture/architecture.json`: declared edges, acyclicity, external allowlists, and the exception ratchet (including the sealed-module rules that keep the migration ledger derived from the graph rather than from memory). |
| `arch-public-headers` | Every published header compiles alone, in a consumer that links only its own module. |
| `arch-selftest` | Nineteen synthetic projects: each rule is shown accepting its allowed graph and rejecting its forbidden one, so a permanently green checker cannot masquerade as coverage. |

---

## Workflow coverage — W01 to W05

No suite runs a complete journey yet. Recording that plainly is the point of
this table: the contributing suites below prove the pieces, not the trip. The
roadmap places W01/W03/W04/W05 in phase 3 and W02 in phase 4.

| Journey | Contributing suites | What is still missing |
| --- | --- | --- |
| **W01** First launch | `engine-discovery` (managed engine resolution and provenance), `core-process` (start → ready → stop with a real child), `runtime-coordinator` (autostart deferred into the event loop, generation as the start action), `profile-store` (selection survives a reopen), `shutdown-coordinator` (no owned child survives quit) | Nothing walks empty workspace → import → start → inspect → stop → quit → reopen in one process against one data directory. |
| **W02** Subscription update | `profile-store` (`rejectsNonHttpSubscriptions`, `subscriptionUrlEditPreservesCacheAndRejectsOldRefresh`, `reloadCancelsInFlightImport`), `config-generation` (overrides survive regeneration), `provider` (subscription counts) | No journey drives a local HTTP subscription through refresh with a user override in place, and no pinned real-core smoke. |
| **W03** Routing controls | `routing-controller` (`everyRoutingSurfaceReadsTheSameConfirmedSystemProxyState`, `theTrayActionAndTheHotkeyUseTheSameIntentPath`), `routing-controls`, `tray`, `proxy`, `system-proxy-async`, `privileged-service`, `controller` (TUN confirmation) | The surfaces are proven to agree on the confirmed value at coordinator level, but not across a started managed backend and a real quit. |
| **W04** Restore | `backup` (snapshot → restore → rollback), `backup-coordinator` (writers quiesced, core stopped before restore, enhancer reloaded before profiles), `runtime-maintenance` (the only evidence for the "writers quiesced" contract) | Nothing reopens the application after a restore to show the restored state is what comes back. |
| **W05** Recovery | `backend-contract` / `backend-real-contract` (supersession, unconfirmed stop, validation failure leaves the running config intact), `core-process` (`hangingProbeCanStopAndRestart`), `shutdown-coordinator` (bounded shutdown, accurate cleanup status) | No journey drops a controller or crashes a child and then switches profile and quits. |

---

## Suites built after this index was first written

Ten suites postdate the partition record below. They are indexed above; this
section is the detail the older suites already have.

### `backend-contract` — `tests/contracts/backend/backend_contract_test.cpp`

* **Features:** F04, F05, F06, F07. The backend-r3 acceptance set.
* **Environment:** headless. Runs against `clash_backend_fake`, a deterministic
  in-process backend — no child processes, no sockets.
* **Cases (28):** published shape/ABI readiness, ownership reporting, generation
  and supersession, readiness requiring a version string rather than a started
  process, validation failure leaving the running configuration intact,
  re-entrancy (no observer invoked from inside a mutating call, safe removal
  during delivery), TUN read-back rather than echo, provider coalescing,
  privileged-service status, actionable failure for a missing managed engine.

### `backend-real-contract` — `tests/core/mihomo/backend_real_contract_test.cpp`

* **Features:** F04, F05, F06, F07.
* **Environment:** real child processes and a real loopback controller, but a
  **fixture** core binary (`clash-qt-fake-core`, passed as
  `CLASH_QT_FAKE_CORE`). It is not evidence about mihomo.
* **Cases (26):** the same semantics as `backend-contract`, plus the readiness
  hard cap not being refreshed by log output, a cancelled readiness probe
  disconnecting before it aborts, and a privileged-service status answered
  without a second connection.

### `backend-real-core` — `tests/core/mihomo/backend_real_core_test.cpp`

* **Features:** F04, F05.
* **Environment:** drives the **locally built engine** (`CLASH_QT_CORE_BINARY`).
  Labelled `real-core`, which `make test` excludes and `make test-integration`
  selects, so an absent engine cannot fail the routine lane — and cannot be
  mistaken for a pass either.
* **Cases (5):** the real engine supervised through the published contract;
  detach and stop reaching only what this component started; a completion
  aborted by an endpoint change marked superseded; a validation failure leaving
  the real engine running; delivery never re-entrant and observer removal safe.

### `engine-discovery` — `tests/core/mihomo/engine_discovery_test.cpp`

* **Features:** F04, F10.
* **Environment:** headless, filesystem only.
* **Cases (8):** the managed path never resolving `PATH` or another Clash
  installation; a local build being a managed engine and labelled as one; an
  unusable local build not resolving; provenance tied to the artefact it
  describes; an engine without a manifest saying so rather than staying silent;
  an explicit choice reported as a choice; distinct user-facing labels.

### `backend-bridge` — `tests/contracts/backend/bridge/backend_bridge_test.cpp`

* **Features:** F04, F05, F06, F07. The Qt-native view of the backend contract
  that the G2 UI migration consumes.
* **Environment:** headless, against the fake backend.
* **Cases (25):** sink registration, state cached from events rather than from
  the backend, every signal firing with its arguments, republication of each
  event family, spans materialised into owned containers, no emission from
  inside a mutating call, superseded completions and stream samples dropped
  while the stop outcome is never dropped, generation announced before the
  events it invalidates, disconnection and receiver destruction during emission.

### `component-contract` — `tests/contracts/component/component_contract_test.cpp`

* **Features:** F10 (component ABI, G2).
* **Environment:** headless. No Qt event loop required.
* **Cases (30):** signed result codes and their published values, published
  interface ids matching the contract table, query reflexivity/symmetry/
  stability and identity across interfaces, reference-count behaviour on
  success and failure, smart-pointer adopt/retain/move/put/detach, atomic
  reference counting under concurrency, buffer ownership and failing resize,
  error info retrieved from the failing object, unknown class distinguished
  from unknown interface.

### `shutdown-coordinator` — `tests/app/lifecycle/shutdown_coordinator_test.cpp`

* **Features:** F04, F05, F10.
* **Environment:** headless, no widget tree.
* **Cases (16):** quit held until approved and approval deferred by one event
  loop turn; repeated quit requests running the actions once; system-proxy
  shutdown strictly preceding the core stop; a failed proxy restore still
  stopping the core; an earlier stop not satisfying the quit's own stop; busy
  gates evaluated in declared order; the final prune running exactly once; an
  unconfirmed stop blocking quit and never being reported as success; a stop
  completion from a superseded generation rejected.

### `backup-coordinator` — `tests/app/backup/backup_coordinator_test.cpp`

* **Features:** F08, F01, F03.
* **Environment:** headless, no widget tree — the store is owned rather than
  discovered in the widget tree, which is one of the assertions.
* **Cases (11):** preparation required from the start and waiting until both
  stores have finished writing; restore preparation stopping the core first,
  with a synchronous fast path when it is already stopped; an unrelated core
  stop not resuming a restore; maintenance mode following the operation and the
  shutdown; a restore reloading the enhancer before the profiles.

### `runtime-coordinator` — `tests/app/runtime/runtime_coordinator_test.cpp`

* **Features:** F03, F04, F01.
* **Environment:** headless, against the fake backend.
* **Cases (13):** the reload gate being the backend's own predicate for every
  state; a 100 ms reload debounce that coalesces triggers; a core stopping
  inside the debounce window not being reloaded; a reload with nothing selected
  stopping the core instead of regenerating; autostart deferred into the event
  loop; a ready core attached to before its retired snapshot is discarded;
  snapshot deletion never running on the GUI thread; an idempotent final prune.

### `routing-controller` — `tests/app/runtime/routing_controller_test.cpp`

* **Features:** F05. The strongest W03 evidence in the tree.
* **Environment:** headless, against the fake backend.
* **Cases (11):** the shipped restore delays (0 s and 5 s); a stopped core
  restoring only the proxy the application owns; a core that restarts before
  the hop fires, and a controller that returns inside the grace window, both
  keeping their proxy; a TUN change in flight never reported as applied; a TUN
  read-back that disagrees with the request not counting as an applied change;
  every routing surface reading the same confirmed state; the tray action and
  the hotkey using the same intent path.

---

## Counts move, and are meant to

Two workers are migrating the UI onto the published backend contract while this
is written. Case counts and the link ledger in
`tests/architecture/architecture.json` both move with that migration; the
architecture checker derives its G2 site list from the evaluated build graph on
every run rather than from a number recorded here, so a link that appears or
disappears fails loudly instead of silently disagreeing with this file. Treat
per-suite case counts in the sections below as accurate at the partition, not as
invariants — the original-to-new maps are the invariant.

One count has already moved: `core-process` gained two cases after the
partition, when privileged-service mode was restored (`d697d45`). It is 6 cases
now, not the 4 recorded below; the 19 originals are all still there.

---

## Suites from `tests/core/runtime_test.cpp` (was CTest `runtime`, 19 cases)

### `config-generation` — `tests/core/config_generation_test.cpp`

* **Features:** F03 (primary), F05, F01 (one documented straddling assertion).
* **Environment:** `QT_QPA_PLATFORM=offscreen`. No network, no child processes.
  Scoped temporary data directory per test function.
* **Cases (8):** `runtimeAppliesEnhancementsAndProtectsController`,
  `runtimeTunDefaultsAreDisabledAndRoutable`,
  `runtimeTunPreservesProfileAndOverrideChoices`,
  `malformedTunDoesNotReplaceRuntime`, `failedScriptPreservesConfigAndContinues`,
  `cyclicYamlIsRejectedWithoutRecursingForever`, `explicitStringTagsSurviveScripts`,
  `runtimeGenerationKeepsEventLoopResponsiveAndDropsStaleResults`.
* **Straddles the boundary:** the last four lines of
  `runtimeAppliesEnhancementsAndProtectsController` are an F01 profiles-persistence
  assertion (overrides and current uid survive a reopen). They stay here rather
  than being rebuilt on a duplicated fixture in `profile-store`. The profiles
  package owns that assertion; it is commented in place.

### `profile-store` — `tests/core/profile_store_test.cpp`

* **Features:** F01, F02, F03 (one documented straddling half).
* **Environment:** `QT_QPA_PLATFORM=offscreen`. Real loopback TCP (`QTcpServer`)
  for the subscription cases. Scoped temporary data directory per test function.
* **Cases (5):** `invalidEditAndRuntimePreserveFiles`,
  `rejectsNonHttpSubscriptions`,
  `subscriptionUrlEditPreservesCacheAndRejectsOldRefresh`,
  `reloadCancelsInFlightImport`,
  `asyncProfileAndEnhancementWritesValidateAndDrainOnShutdown`.
* **Straddles the boundary:** the second half of
  `asyncProfileAndEnhancementWritesValidateAndDrainOnShutdown` repeats the
  identical busy / validate / drain-on-shutdown contract against
  `core::ConfigEnhancer` (F03) instead of `core::ProfileStore`. It is two tests in
  one body and is kept whole; the config package owns the second half. Commented
  in place.

### `core-process` — `tests/core/core_process_test.cpp`

* **Features:** F04, and F09 since the privileged-service-mode cases landed.
* **Environment:** `QT_QPA_PLATFORM=offscreen`. **POSIX only** — every case is
  `QSKIP`ped under `Q_OS_WIN`. Needs `/bin/sh`, and
  `restartWaitsForOldExitWithoutBlockingGui` needs `/usr/bin/python3`. Uses real
  loopback TCP, a real `QLocalServer` and real child processes; slowest of the
  core suites (~7.5 s).
* **Cases at the partition (4):** `restartWaitsForOldExitWithoutBlockingGui`,
  `privilegedServiceLifecycleUsesLeaseAndWaitsForStopAck`,
  `validationIsResponsiveAndCancelable`, `hangingProbeCanStopAndRestart`.
* **Added afterwards (2, now 6 in total):**
  `anUninjectedProcessHasNoPrivilegedServiceAndSaysSo` and
  `anInjectedAdapterMakesServiceModeAvailableWhereThePlatformSupportsIt`, from
  `d697d45` "restore privileged-service mode, which P3 silently disabled". They
  are new coverage, not partitioned cases, so they are outside the 19-original
  accounting below and are listed here rather than in the original-to-new map.
* `privilegedServiceLifecycleUsesLeaseAndWaitsForStopAck` also drives
  `platform::PrivilegedServiceClient`, but its oracle throughout is
  `core::CoreProcess::state()`, so it belongs here and not in the platform lane.

### `runtime-maintenance` — `tests/core/maintenance_test.cpp`

* **Features:** F08, F01. The only evidence in the tree for the F08 "writers
  quiesced before restore" contract.
* **Environment:** `QT_QPA_PLATFORM=offscreen`. No network, no child processes.
* **Cases (2):** `maintenanceCancelsFileWritesBeforeRestoreCanProceed`,
  `maintenanceBlocksMutationsAndCancelsRuntimeWork`.
* **Logical home is `tests/app/runtime/`.** Both cases drive the profile store and
  the enhancement chain together as a coordinator contract, not as a core unit.
  The file is parked under `tests/core/` only because the application package has
  no test directory yet. Moving it is a path change with no edit to the file.

### Original-to-new map — `runtime`

| Original case | New suite |
| --- | --- |
| `runtimeAppliesEnhancementsAndProtectsController` | `config-generation` (F01 tail documented in place) |
| `runtimeTunDefaultsAreDisabledAndRoutable` | `config-generation` |
| `runtimeTunPreservesProfileAndOverrideChoices` | `config-generation` |
| `malformedTunDoesNotReplaceRuntime` | `config-generation` |
| `failedScriptPreservesConfigAndContinues` | `config-generation` |
| `invalidEditAndRuntimePreserveFiles` | `profile-store` |
| `cyclicYamlIsRejectedWithoutRecursingForever` | `config-generation` |
| `rejectsNonHttpSubscriptions` | `profile-store` |
| `subscriptionUrlEditPreservesCacheAndRejectsOldRefresh` | `profile-store` |
| `reloadCancelsInFlightImport` | `profile-store` |
| `explicitStringTagsSurviveScripts` | `config-generation` |
| `asyncProfileAndEnhancementWritesValidateAndDrainOnShutdown` | `profile-store` (F03 half documented in place) |
| `maintenanceCancelsFileWritesBeforeRestoreCanProceed` | `runtime-maintenance` |
| `runtimeGenerationKeepsEventLoopResponsiveAndDropsStaleResults` | `config-generation` |
| `maintenanceBlocksMutationsAndCancelsRuntimeWork` | `runtime-maintenance` |
| `restartWaitsForOldExitWithoutBlockingGui` | `core-process` |
| `privilegedServiceLifecycleUsesLeaseAndWaitsForStopAck` | `core-process` |
| `validationIsResponsiveAndCancelable` | `core-process` |
| `hangingProbeCanStopAndRestart` | `core-process` |

19 originals, 19 destinations. Nothing merged, nothing dropped.

---

## Suites from `tests/ui/data_pages_test.cpp` (was CTest `data-pages`, 17 cases)

All of these construct real production widgets and isolate `core::preferences`
through `testsupport::ScopedEnvironment` in `initTestCase()`; `cleanupTestCase()`
fails if the developer's real preference store changed during the run.

### `home-page` — `tests/ui/home_page_test.cpp`

* **Features:** F07, F10.
* **Environment:** `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`
  (embeds a native `QQuickView`). Needs the `ClashQt` QML module.
* **Cases (2 functions, 3 invocations):** `homeCardsDoNotOverlap` (data-driven:
  rows `minimum-684x450` and `default-944x590`), `trafficWheelScrollsHomePage`.
* `CLASH_QT_AUDIT_IMAGES` is honoured but is **not** a gate: unset, the case still
  asserts everything and simply writes no image.

### `traffic-graph` — `tests/ui/traffic_graph_test.cpp`

* **Features:** F07, F10.
* **Environment:** `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`.
  Needs the `ClashQt` QML module.
* **Cases (2):** `trafficScrollsBetweenSamplesAndStopsWhenPausedOrHidden`,
  `trafficNativeWindowFollowsLayoutAndPageVisibility`.
* Both were kept. They overlap on one assertion only — that `animate` goes false
  when the graph is hidden — and each surrounds it with a different regression:
  the first proves frames stop being produced, the second proves the native window
  is hidden and restored with page switching and is destroyed with the widget.
  Neither subsumes the other.
* `ui::theme::install()` is **not** called by this suite, matching the original.
  Adding it would change metrics that the geometry assertions depend on, so the
  inconsistency with `home-page` is recorded rather than silently normalised.

### `proxies-page` — `tests/ui/proxies_page_test.cpp`

* **Features:** F06.
* **Environment:** `QT_QPA_PLATFORM=offscreen`. No QML module.
* **Cases (3):** `emptyProxySnapshotClearsMembersAndAction`,
  `proxyFilterAndLatencySort`, `proxySnapshotsSkipUnchangedAndHiddenRebuilds`.
* `ui::ProxiesPage` persists `proxies/sort`, which is why the preference isolation
  in `initTestCase()` is load-bearing here and not merely hygiene.

### `connections-page` — `tests/ui/connections_page_test.cpp`

* **Features:** F07.
* **Environment:** `QT_QPA_PLATFORM=offscreen`. No QML module.
  `connectionDetailsCanBeCopied` **writes the process clipboard** — a
  process-global side effect, so this suite must not be merged with one that reads
  the clipboard.
* **Cases (5):** `hiddenConnectionsStillCaptureRatesAndClosedHistory`,
  `connectionSnapshotPreservesSelectedId`,
  `connectionRatesSortAndResetOnCounterRollback`,
  `closedHistoryIsBoundedAndHasFrozenDuration`, `connectionDetailsCanBeCopied`.

### `providers-page` — `tests/ui/providers_page_test.cpp`

* **Features:** F06.
* **Environment:** `QT_QPA_PLATFORM=offscreen`. Binds a loopback `QTcpServer`
  purely to give the client a valid endpoint; nothing is served over it.
* **Cases (1):** `providersSkipHiddenAndUnchangedRebuilds`.

### `rules-page` — `tests/ui/rules_page_test.cpp`

* **Features:** F06.
* **Environment:** `QT_QPA_PLATFORM=offscreen`.
* **Cases (1):** `rulesKeepRoutingOrderAndFilterType`.

### `logs-page` — `tests/ui/logs_page_test.cpp`

* **Features:** F07.
* **Environment:** `QT_QPA_PLATFORM=offscreen`. Needs `Qt6::Concurrent`
  (`logs_page.cpp` uses `QtConcurrent::run`).
* **Cases (1):** `logsFilterExistingHistoryAndEscapePayload`.

---

## Benchmark lane — `tests/benchmarks/`

Both entries carry the `benchmark` label, which the `make test` lane excludes.
This is the point of moving them: in the old `data-pages` suite they were
registered in the routine lane and reported **"Passed" on every run while in fact
skipping**, because CTest never sets their environment flag and the lane is
offscreen. They are now honestly *not run* instead of falsely counted.

**Their `add_test` entries must not set `QT_QPA_PLATFORM=offscreen`.** With the
flag set and the platform forced offscreen, the platform check fails rather than
skips, so an opt-in run would report a spurious failure.

### `traffic-graph-frames` — `tests/benchmarks/traffic_graph_frames_test.cpp`

* **Features:** F07.
* **Labels:** `benchmark native ui pages`.
* **Environment:** requires `CLASH_QT_VERIFY_GRAPH_FRAMES` to be set **and** a
  native (non-offscreen) GPU display. Needs the `ClashQt` QML module.
  `CLASH_QT_AUDIT_IMAGES` optionally dumps frames.
* **Cases (1):** `trafficNativeFillStaysBelowOutline`.
* **Known skip.** Without the flag it skips with
  `"Set CLASH_QT_VERIFY_GRAPH_FRAMES=1 on a native GPU display"` — the same skip,
  for the same reason, as before the split.
* **Misfiled on purpose, and it should move.** This is a rendering *correctness*
  test with hard assertions, not a measurement: it grabs 60 frames, inspects pixel
  colours and requires `invalidFrames == 0`. It belongs in a `native` lane, not a
  performance lane. `tests/benchmarks/` is its interim home because no `native`
  directory exists yet; its entry carries the `native` label for that reason.

### `traffic-frame-pacing` — `tests/benchmarks/traffic_frame_pacing_test.cpp`

* **Features:** none — infrastructure measurement.
* **Labels:** `benchmark native ui pages`.
* **Environment:** requires `CLASH_QT_MEASURE_FRAMES` to be set **and** a native
  (non-offscreen) display. Needs the `ClashQt` QML module.
* **Cases (1):** `trafficNativeFrameTiming`.
* **Known skip.** Without the flag it skips with
  `"Set CLASH_QT_MEASURE_FRAMES=1 on a native display to measure frame pacing"` —
  the same skip, for the same reason, as before the split.
* It still **prints** screen, refresh rate, FPS, mean, median and p95 and
  **records** none of it, and records no workload, hardware, warm-up or duration
  metadata. That gap predates the move and is unchanged by it.

### Original-to-new map — `data-pages`

| Original case | New suite |
| --- | --- |
| `homeCardsDoNotOverlap` (2 data rows) | `home-page` |
| `trafficScrollsBetweenSamplesAndStopsWhenPausedOrHidden` | `traffic-graph` |
| `trafficNativeWindowFollowsLayoutAndPageVisibility` | `traffic-graph` |
| `trafficWheelScrollsHomePage` | `home-page` |
| `trafficNativeFillStaysBelowOutline` | `traffic-graph-frames` (benchmark lane) |
| `trafficNativeFrameTiming` | `traffic-frame-pacing` (benchmark lane) |
| `emptyProxySnapshotClearsMembersAndAction` | `proxies-page` |
| `proxyFilterAndLatencySort` | `proxies-page` |
| `proxySnapshotsSkipUnchangedAndHiddenRebuilds` | `proxies-page` |
| `hiddenConnectionsStillCaptureRatesAndClosedHistory` | `connections-page` |
| `providersSkipHiddenAndUnchangedRebuilds` | `providers-page` |
| `connectionSnapshotPreservesSelectedId` | `connections-page` |
| `connectionRatesSortAndResetOnCounterRollback` | `connections-page` |
| `closedHistoryIsBoundedAndHasFrozenDuration` | `connections-page` |
| `connectionDetailsCanBeCopied` | `connections-page` |
| `rulesKeepRoutingOrderAndFilterType` | `rules-page` |
| `logsFilterExistingHistoryAndEscapePayload` | `logs-page` |

17 originals, 17 destinations. Nothing merged, nothing dropped.

---

## Accounting

CTest's `Totals:` line counts `initTestCase` and `cleanupTestCase` as passing
functions. Every new executable has its own pair, so the *reported* totals must
grow by two per suite even though no case was added. The invariant that actually
holds is the **case count**.

### `runtime` — 19 cases

Before: `Totals: 21 passed` = 19 cases + `initTestCase` + `cleanupTestCase`.

After, measured:

| Suite | Reported | Framework | Cases |
| --- | --- | --- | --- |
| `config-generation` | 10 passed | 2 | 8 |
| `profile-store` | 7 passed | 2 | 5 |
| `core-process` | 6 passed | 2 | 4 |
| `runtime-maintenance` | 4 passed | 2 | 2 |
| **Sum** | **27 passed** | **8** | **19** |

`27 − (4 × 2) = 19`, and `19 + 2 = 21` — the original total.

Measured at the partition. `core-process` has since gained the two
privileged-service-mode cases described above, so it now reports 8 passed / 6
cases and the row sum is 29 passed / 21 cases. The invariant the table asserts
is unaffected: it is that the 19 originals all survived, not that the total can
never grow.

### `data-pages` — 17 cases / 18 invocations

Before: `Totals: 18 passed, 2 skipped` = 16 passing invocations (17 cases, one
data-driven with 2 rows, minus the 2 that skipped) + `initTestCase` +
`cleanupTestCase`, plus the 2 skips.

After, measured:

| Suite | Reported | Framework | Invocations |
| --- | --- | --- | --- |
| `home-page` | 5 passed | 2 | 3 |
| `traffic-graph` | 4 passed | 2 | 2 |
| `proxies-page` | 5 passed | 2 | 3 |
| `connections-page` | 7 passed | 2 | 5 |
| `providers-page` | 3 passed | 2 | 1 |
| `rules-page` | 3 passed | 2 | 1 |
| `logs-page` | 3 passed | 2 | 1 |
| **Sum (routine lane)** | **30 passed** | **14** | **16** |
| `traffic-graph-frames` | 2 passed, 1 skipped | 2 | 1 (skips) |
| `traffic-frame-pacing` | 2 passed, 1 skipped | 2 | 1 (skips) |
| **Sum (all)** | | **18** | **18** |

`30 − (7 × 2) = 16` running invocations, `+ 2` skipping invocations in the
benchmark lane `= 18`, and `16 + 2 = 18 passed` with `2 skipped` — the original
totals. The two skips no longer appear in `make test`, by design: the `benchmark`
label excludes them, so they are not counted at all rather than counted as passes.

### Known skips

| Case | Suite | Skips unless |
| --- | --- | --- |
| `trafficNativeFillStaysBelowOutline` | `traffic-graph-frames` | `CLASH_QT_VERIFY_GRAPH_FRAMES` is set and the platform is not `offscreen` |
| `trafficNativeFrameTiming` | `traffic-frame-pacing` | `CLASH_QT_MEASURE_FRAMES` is set and the platform is not `offscreen` |
| every `core-process` case (6) | `core-process` | the platform is not Windows |

---

## Shared helpers

Two header-only files, each extracted only because it has several consumers.
Anything used by a single partition stayed private to that partition. The
compiled fixtures they sit beside — `scoped_environment`, `loopback_server`,
`fake_core` and the `clash-qt-fake-core` helper binary — have self-tests of
their own; see "Fixture self-tests" in the registered-suite index.

### `tests/support/fixture_files.h`

`testsupport::writeFile()` / `testsupport::readFile()` — write a YAML fixture and
read an artefact back byte-for-byte.

* **Consumers (4):** `config-generation`, `profile-store`, `core-process`,
  `runtime-maintenance`.
* Carried over verbatim from the monolith's anonymous namespace, including the
  quirk that a `QVERIFY` failure inside `writeFile` returns from the helper rather
  than from the calling test.

### `tests/support/preference_isolation.h`

`testsupport::preferenceIsolationFailure()` /
`testsupport::preferenceEscapeFailure()` — the checks the monolith made inline in
`initTestCase()` / `cleanupTestCase()`: the scoped environment is valid,
`core::preferences` really resolved inside it, and the developer's real preference
store is unchanged at the end.

* **Consumers (12):** all seven `ui/pages` suites, both benchmark suites, and
  the three application-coordinator suites (`backup-coordinator`,
  `runtime-coordinator`, `routing-controller`) added afterwards.
* They return a message instead of asserting, because a `QVERIFY2` inside a helper
  would return from the helper and hide the failure.

Neither header adds anything to link, and neither modifies the existing
`scoped_environment`, `loopback_server` or `fake_core` fixtures.

## Fixture change made while extracting

The core suites now take one `testsupport::ScopedEnvironment` per test function in
place of the monolith's raw `QTemporaryDir` plus
`qputenv`/`qunsetenv("CLASH_QT_DATA_DIR")`. The old `cleanup()` *unset* the
variable instead of restoring it, so a developer running with `CLASH_QT_DATA_DIR`
exported lost it for the rest of the process after the first case.
`ScopedEnvironment` restores the prior value, including "was not set at all".

## Known gaps carried over, not introduced

* `core-process` still builds its fake cores as inline `/bin/sh` and
  `/usr/bin/python3` scripts and therefore still skips on Windows, even though
  `clash_test_support` now ships a compiled `clash-qt-fake-core` helper built for
  exactly this. Converting them is a behaviour change, not a partition, and was
  left for the owner of that suite.
* `core-process` and `config-generation` still assert machine-dependent wall-clock
  budgets (`< 200 ms`, `< 500 ms`) inside functional tests.
