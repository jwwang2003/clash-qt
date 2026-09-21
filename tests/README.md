# Test suite index

Coverage index for the suites produced by partitioning the two shared monoliths
`tests/core/runtime_test.cpp` and `tests/ui/data_pages_test.cpp`. Both files are
gone; every case they held lives in exactly one suite below. Use the
original-to-new maps to confirm that nothing was dropped.

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

* **Features:** F04.
* **Environment:** `QT_QPA_PLATFORM=offscreen`. **POSIX only** — every case is
  `QSKIP`ped under `Q_OS_WIN`. Needs `/bin/sh`, and
  `restartWaitsForOldExitWithoutBlockingGui` needs `/usr/bin/python3`. Uses real
  loopback TCP, a real `QLocalServer` and real child processes; slowest of the
  core suites (~7.5 s).
* **Cases (4):** `restartWaitsForOldExitWithoutBlockingGui`,
  `privilegedServiceLifecycleUsesLeaseAndWaitsForStopAck`,
  `validationIsResponsiveAndCancelable`, `hangingProbeCanStopAndRestart`.
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
| all four `core-process` cases | `core-process` | the platform is not Windows |

---

## Shared helpers

Two header-only files, each extracted only because it has several consumers.
Anything used by a single partition stayed private to that partition.

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

* **Consumers (9):** all seven `ui/pages` suites and both benchmark suites.
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
