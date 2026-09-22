# P4 ISOLATED CONTROLLER DISCOVERY — registration and proof notes

Worker: Opus (claude-opus-5). No sub-agents. Scratch: `/tmp/clash-qt-p4.w0Y8Uo/isolation`.
Prefs plist hash at lease start and at every run boundary:
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` — unchanged in all 17
logged runs (`evidence/run-*.log`, `evidence/audit-*.log`, each records before AND after).

## Files written (the only writes of this lease)

| file | change |
| --- | --- |
| `src/core/mihomo/controller_discovery.h` | `ControllerDiscoveryInputs`, `processDiscoveryInputs()`, `noControllerEndpoint()`, `discoverEndpoint(inputs)`, `endpointFromAuthority()`. Existing signatures unchanged. |
| `src/core/mihomo/controller_discovery.cpp` | isolation gate, authoritative explicit controller, checked authority parser, `foreignConfigPath` left empty when isolated. Legacy branch intact. |
| `src/core/mihomo/mihomo_client.cpp` | `openStream()` only dials when `endpoint_.isValid()`; the socket is still created, wired and returned. Previous epoch/stale-reply work untouched. |
| `tests/core/mihomo/engine_discovery_test.cpp` | 7 controller cases (22 invocations with the engine half) + 3 helpers, between `>>> BEGIN/END MIRRORED CONTROLLER CASES/HELPERS` markers. |
| `tests/workflows/app_smoke_test.cpp` | `theIsolatedLaunchAttachesToNoControllerItWasNotGiven`, `everyChildEnvironmentIsExplicitlyIsolated`, plus header/case comments. |
| `tests/workflows/composition_root_audit.h` | new SOURCE REQUIREMENT `cb::isValid(startupEndpoint)`; strengthened `bridge.attach()` / `bridge.openTrafficStream()` inventory entries. |

## Build registrations

**None required.** `engine-discovery-tests` already links `clash_mihomo_impl` (owns
`controller_discovery.cpp` and `mihomo_client.cpp`) and `clash_test_support` (owns
`loopback_server.cpp`, links `Qt6::Network` PUBLIC). `Qt6::WebSockets` is a PRIVATE
dependency of the static `clash_mihomo_impl`, which CMake exposes as
`$<LINK_ONLY:Qt6::WebSockets>` on its interface. That propagation is DEMONSTRATED, not
assumed: `controller-discovery-mirror-tests` in the snapshot links `snapshot_core`
(WebSockets PRIVATE) without naming WebSockets, and links clean.

`app_smoke_test.cpp` is already registered; the two new cases need no new target or
environment entry. `composition_root_audit.h` is header-only and already on the path.

## Evidence

Snapshot build: `cmake -S snapshot -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`, Qt 6.11.1.
The repository was never configured or built. `build/` and `build-dev/` untouched.

The external suite compiles the *same text* as the repository file:
`mirror.py` extracts the marked regions from
`tests/core/mihomo/engine_discovery_test.cpp` and regenerating it after the final edit
produced an identical file.

Baseline: 22 passed / 0 failed, four times (`run-baseline`, `run-repeat-1..3`, `run-final`).

### Inversions, all on the external copy (`evidence/inversions*.json`, `evidence/run-M*.log`)

| id | damaged claim | detected by |
| --- | --- | --- |
| M1 | isolation gate removed | `anIsolatedProcessWithNoExplicitControllerAttachesToNothing` — "resolved a controller at 127.0.0.1:9090" (and a second failure in the ordinary-launch case) |
| M2 | bad explicit controller falls through | `aBadExplicitControllerResolvesToNothingAndNeverFallsBack` — 7 of 9 rows resolved the LOCAL DECOY 127.0.0.1:29412 |
| M3 | dial regardless of endpoint validity | `aClientWithNoAddressDoesNotDialAndKeepsItsSubscription` — "Stream /traffic failed: Host not found" |
| M4 | return nullptr instead of the undialed socket | same case — the traffic stream never opened after `setEndpoint()` to the fixture |
| M5 | legacy discovery deleted | `anOrdinaryLaunchStillFindsTheNeighbouringInstallation` |
| M6 | bracket handling removed from the parser | `aBadExplicitControllerResolvesToNothingAndNeverFallsBack(bracket never closed)` — `[::1:9090` resolved |
| M7 | `foreignConfigPath` assembled while isolated | `anIsolatedProcessWithNoExplicitControllerAttachesToNothing` |
| M8 | `cb::isValid(startupEndpoint)` deleted from a COPY of `main.cpp` | `compositionRootDrift()` names the requirement and its consequence |
| M9 | `bridge.openTrafficStream()` deleted from that copy | `compositionRootDrift()` — proves the unconditional subscription is still held |

Every "toward" direction is either "attach to nothing" or a decoy inside the test's own
temporary scope (127.0.0.1:29411-29420, or a `decoy.yaml` this suite wrote). Nothing was
pointed at the installed privileged helper, at 127.0.0.1:9090, or at a real controller.
M8/M9 mutate `/tmp/clash-qt-p4.w0Y8Uo/isolation/inverted-root/src/main.cpp`, a copy; the
repository `src/main.cpp` was read only.

### Compile checks of the files I cannot run

`moc` + `clang++ -fsyntax-only -std=c++20` against the repository include paths, writing
only into scratch: `tests/workflows/app_smoke_test.cpp` exit 0, and the full
`tests/core/mihomo/engine_discovery_test.cpp` (engine half included) exit 0.
`evidence/syntax-*.log`.

## Not verified by execution

* The two new app-smoke cases. They need `CLASH_QT_APP_BINARY`, i.e. a built `clash-qt`,
  and building repository sources is forbidden while other leases are active. They are
  compiled and reviewed, not run. The coordinator must run app-smoke on the rebuilt
  binary before this gate closes.
* No Windows/Linux evidence. `vergeConfigPath()`'s non-macOS branches are unchanged and
  uncompiled here.

## Accepted user-visible consequence, for the ledger

`clash-qt --data-dir X` no longer auto-discovers a neighbouring installation's controller.
That is the architecture decision, not a defect: such a launch must name its controller
with `CLASH_QT_CONTROLLER`. An ordinary launch (no `--data-dir`) is unchanged.

## Stale comment outside my lease

`src/core/mihomo/mihomo_client.h` (~line 56) says "Discovery still hands that default out
(controller_discovery.cpp:73-77)". Still true for an ordinary launch, but the line numbers
have moved and it no longer holds for an isolated one. The header is not mine to edit.
