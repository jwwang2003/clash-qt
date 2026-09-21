# Test architecture and redesign plan

Status: proposed, 2026-09-20. This is a design and migration plan, not a test run
or a claim that the proposed coverage exists.

The [sub-agent execution plan](PARALLEL_EXECUTION_PLAN.md) assigns each feature
worker its implementation and adjacent tests; shared fixtures, architecture checks
and complete workflows have explicit owners. Combined-tree gates belong to the
coordinator. Test work is integrated into every wave rather than deferred to QA.

The component baseline is now the portable COM design informed by
`3rdparty/ref/fxcom`, without FX prefixes, on every OS. Native Microsoft COM is
optional. F11–F13/W06–W08 below cover the requested monitor and MITM add-ons in
[CAPTURE_MODULE_PLAN.md](CAPTURE_MODULE_PLAN.md); they are new scope, not existing
passing coverage.

## Purpose

The suite should demonstrate that clash-qt's components are independently usable,
its major features satisfy their contracts, and the assembled application completes
normal user workflows. Reported bugs supply examples within those contracts; they
must not determine the entire test strategy.

Organize tests by the feature or boundary they protect. Use CTest labels for test
level and environment. A useful test states observable behavior and uses an oracle
independent of the implementation, rather than checking private method calls or
reproducing the production algorithm. Keep existing regressions when they protect
distinct behavior; consolidate overlapping cases into data-driven scenarios.

## Current evidence and gaps

The current CMake configuration defines 12 Qt test executables and two macOS
helper self-test entries. The latter are native helper tests, not Qt test suites.
The suite is broader than bug checks alone: it already exercises configuration
precedence, persistence, backup round trips, authenticated controller requests,
provider operations, proxy ownership, bounded telemetry and UI interactions.

| Area | Evidence in the current suite | Gap to address |
| --- | --- | --- |
| Configuration and profiles | `runtime_test.cpp`: enhancement precedence, protected fields, persistence, invalid input and asynchronous generation | Separate config/profile/process contracts; cover a normal subscription lifecycle and feature combinations |
| Core execution | Runtime tests drive fake processes and a fake helper | Complete application lifecycle, portable process fixture, actual mihomo compatibility |
| Controller and providers | REST fixtures, TUN readback, endpoint changes, provider listing/mutations | Real WebSocket framing/auth/reconnect paths; fake/real API agreement |
| Backup | Archive round trip, restore gating, rollback and WebDAV fixture | End-to-end backup/restore with the assembled application and restart persistence |
| System proxy | Mocked macOS command transactions and async operation tests | Explicit backend contract on every supported OS; native validation separately |
| UI | Data-page, tray, routing and dashboard tests | Main-window workflows; profiles/settings/backups behavior; consistent loading/empty/error states |
| Architecture | Production source lists repeated in test executables | Dependency rules, public-header checks, headless core build and component replacement |
| Distribution | Helper self-tests; QML exercised in widget tests | Repeatable installed-artifact launch and resource/dependency checks |

Specific seams need redesign: the proxy test includes a production `.cpp` and
accesses internal globals; the dashboard test substitutes production symbols;
one helper timeout test finds and changes an internal QTimer. Several process
tests require POSIX scripts or `/usr/bin/python3` and skip on Windows. Some UI
tests inject MihomoClient signals directly: that is valid presentation coverage,
but does not prove controller parsing or transport. Native frame measurement is
currently embedded in the ordinary data-page suite behind an environment flag.

No architecture tests, application executable workflow tests, or real-core CI
target were found in the reviewed test registration. No checked-in CI workflow
was found. These are implementation gaps, not claims about manual verification.

## Test layout

Extend the layout in [the source refactor plan](STRUCTURE_REFACTOR_PLAN.md):

```text
tests/
  CMakeLists.txt
  architecture/             Dependency checks and public API build probes
  contracts/                Reusable backend/provider interface behavior
  core/
    config/                 YAML, composition, enhancements
    profiles/               Storage, subscriptions, refresh
    mihomo/                 REST, WebSockets, execution
    backups/                Archive and WebDAV
    telemetry/              History/statistics
  app/                      Runtime, routing, backup and shutdown coordinators
  platform/                 OS adapters and helper IPC
  ui/
    shell/                  Navigation, tray, shared controls
    pages/                  Page behavior, grouped by feature as needed
    widgets/                Shared controls
  workflows/                Multi-component and executable journeys
  packaging/                Installed artifact checks
  native/                   Display/network integration scenarios
  benchmarks/               Native frame pacing and load measurements
  support/                  Small fixtures and test executables
  fixtures/                 Synthetic YAML, protocol and archive examples
```

This is a destination map, not a requirement to create empty folders or a test
executable for every class. Separate targets where dependencies or environments
differ. Use labels such as `architecture`, `unit`, `contract`, `integration`, `ui`,
`workflow`, `real-core`, `packaging`, `native`, `privileged`, and `benchmark`.
Feature labels can overlap level labels; document CTest selection in presets.

## Architecture checks

Test meaningful boundaries rather than exact filenames or an arbitrary file-count
limit. Keep a small declared module/dependency graph beside CMake and check:

1. **Dependency direction:** core and platform targets do not link UI or app
   targets; core has no Widgets/Quick/Graphs dependency. Platform adapters may use
   Qt Gui where native hotkeys/browser behavior requires it. Application services
   have no QWidget dependency. Configuration/telemetry targets do not acquire
   process, HTTP or GUI dependencies merely through broad umbrella targets.
2. **Build graph:** inspect evaluated CMake target dependencies, including transitive
   links, and reject cycles/forbidden edges. Supplement with project-include checks
   for forbidden layer access; do not claim an include scanner proves all coupling.
   Inspect compiled dependencies where practical, so generated and indirect includes
   are not silently omitted. During migration, baseline named existing exceptions
   with an owning removal phase; prohibit adding new exceptions casually.
3. **Public interfaces:** compile each published header in a small consumer with
   only its declared target dependencies. Prove clients do not require private
   headers, global include directories or accidental include order.
4. **Headless reuse:** configure and build backend tests without finding Widgets,
   Quick or Graphs. This requires optional desktop dependency discovery in CMake;
   the current unconditional root `find_package` must be restructured first.
5. **Replaceable implementations:** link an app coordinator against a deterministic
   test backend implementing the same public contract. Drive real coordinator
   behavior to show the boundary is usable, beyond merely compiling an interface.

Use CMake-generated metadata/File API for evaluated targets and a small checker;
avoid introducing a large architecture framework. Test the checker with tiny
allowed and forbidden graphs so a permanently green checker cannot masquerade
as architecture coverage. Native helper dependencies remain separately constrained.

## Feature contracts

Track the following coverage IDs in `tests/README.md` when implemented. For each,
record test targets, supported environments, and remaining gaps. The table is the
acceptance scope, not an assertion that every row requires a new test.

| ID / feature | Main success contract | Failure, recovery and interaction contracts |
| --- | --- | --- |
| F01 Profiles | Create/import, select, edit, rename/remove; reload preserves selection and content | Invalid input preserves prior state; removal of active profile follows defined core behavior; concurrent refresh/edit has a documented winner |
| F02 Subscriptions | Fetch from local fixture, read quota/expiry, scheduled refresh updates selected runtime | Auth/HTTP/malformed response leaves usable cache; URL change invalidates old replies; refresh cannot erase separate overrides |
| F03 Configuration | Base + global enhancements + overrides yield expected effective settings and ordered rules | Protected controller fields remain owned; explicit false/empty values survive; script failures follow current fallback semantics; accepted candidate validates with supported mihomo |
| F04 Core lifecycle | Start → ready → change profile → restart → stop, with observable desired/applied state | Validation failure preserves running config; crash/readiness failure reports state; replacement waits for prior exit; external core is never treated as owned |
| F05 Routing | Settings, toolbar, tray and hotkeys share confirmed System Proxy/TUN state | Pending changes do not report success; rejected/partial operations reconcile; restoration respects ownership; app shutdown completes cleanup |
| F06 Proxies/providers/rules | List/filter/select nodes, run latency/health operations, refresh providers; preserve rule order | Encoded identifiers, stale responses, missing selections and read-only groups behave consistently; refresh retains valid user selection |
| F07 Telemetry/logs/connections | Parse streams, display rates/history, inspect and close connections; filter/export logs | Reconnect clears stale epochs; malformed frames do not fabricate valid data; retention is bounded; counter resets/gaps are explicit; pause freezes display while capture continues |
| F08 Backup/restore | Snapshot → modify → restore → reopen reproduces supported configuration and settings | Validation precedes mutation; interrupted/failed restore recovers; writers/core respect gates; excluded secrets stay excluded; WebDAV round trip retains content |
| F09 Preferences/integrations | Settings persist across launch; startup preference, hotkey dispatch and dashboard launch follow selected options | Unavailable binary/browser/helper produces actionable state; failed OS changes preserve confirmed settings; external calls use test adapters |
| F10 Application/distribution | Installed app opens, navigates pages, loads QML/icons, enforces single instance per data directory and shuts down | Missing core, disconnected controller and empty profile states remain usable; isolated data directories do not interfere; no owned child remains after normal quit |
| F11 Metadata monitoring | Selected host/container app has attributed endpoint/direction/volume events in container-backed catalog | Unrelated app excluded; PID reuse/restart, incoming connections, encrypted URL visibility, container/sensor gaps and no body retention |
| F12 MITM capture | Qualified selected-app requests and replies are inspected, saved and forwarded with content preserved | Target trust/pinning/mTLS/app encryption states, invalid upstream TLS, unsupported protocols, body limits, worker/redirector recovery |
| F13 Capture jobs/export | Native capture reopening, bounded exports and persisted scheduled app/script jobs | Disk/quota/partial files, script failure, overlap/DST/sleep/restart, missing target, cancellation and owned-state cleanup |

For applicable features, cover normal use, empty/boundary input, failure, recovery,
persistence and concurrency. Choose cases by consequence and distinct behavior;
do not require every combination for every feature. Use explicit tables for config
precedence, managed/external core state, and routing permissions. Include pairwise
combinations where useful, plus deliberate high-risk combinations such as restore
during pending writes and shutdown during a routing change.

Use independent expectations: hand-authored expected fields and rule ordering,
observable file/settings round trips, actual bytes delivered through a local proxy,
and public state transitions. Normalize only nondeterministic secrets/paths in
golden fixtures. Do not regenerate expected output from the composer being tested,
or compare unordered YAML serialization when semantic equality is the contract.

## Complete workflows

Start with five journeys. Keep most business-state coverage in headless application
integration tests; add a small executable/UI layer to prove actual wiring.

| Journey | Exercise | Observable outcome |
| --- | --- | --- |
| W01 First launch | Isolated empty workspace → import local profile → start → inspect status → stop → quit → reopen | Correct profile persists, readiness is real, owned child exits, application remains usable |
| W02 Subscription update | Local HTTP subscription → selected profile → user override → server update → refresh | New subscription data is applied while override remains; failed refresh preserves the last valid runtime |
| W03 Routing controls | Start managed backend → toggle from settings/toolbar/tray → reject one change → quit | All surfaces agree with confirmed state; failure is visible; only owned proxy state is restored |
| W04 Restore | Create profiles/enhancements/settings → backup → change them → restore while app services exist → reopen | Restored state is consistent; writers/processes were quiesced and no stale reply overwrites it |
| W05 Recovery | Run core → controller drops or child crashes → attempt restart → switch profile → quit | State converges to the latest request, failure is reported, shutdown is bounded with accurate cleanup status |
| W06 Monitor app | Select one of two synthetic apps → send/accept traffic → inspect container-backed events → stop/restart collector | Correct target/direction/provenance; HTTPS paths absent; unrelated app and bodies excluded; gaps visible |
| W07 Inspect app | Configure fixture-only trust → select app → send HTTPS request and reply through worker → save/reopen/export → stop | Payload fidelity and attribution; unsupported/pinned cases accurate; routes/owned session resources cleaned up |
| W08 Scheduled capture | Create bounded job/script → trigger/restart app → simulate overlap/sleep/error → reopen saved result | Stable target resolution, duplicate prevention, explicit completeness/skip/failure and retention behavior |

W01 and a subset of W02/W05 should also run with the local-source mihomo executable
through the packaged component, as required by [G1/G2](PROJECT_GOALS.md).
Use synthetic DIRECT-only configuration, TUN off, loopback controller/proxy ports,
and a local HTTP origin. Send a request through the proxy and verify its response
at the origin; use an unreachable/rejected destination to prove rule behavior.
Do not change system proxy settings in this job. Build `3rdparty/mihomo` at the
recorded source commit in CI setup and verify the staged binary's provenance and
checksum. Never silently discover a user's installed core or fetch a prebuilt core
to satisfy this gate. Missing binaries fail required real-core jobs;
optional local runs report an explicit skip. CI and developer documentation use the
same `make test` and `make test-integration` command surface defined in
[BUILD_RELEASE_PLAN.md](BUILD_RELEASE_PLAN.md).

The executable smoke harness launches the actual installed app with `--data-dir`
and `--no-autostart`, drives its supported single-instance/quit behavior, and checks
process exit and isolated artifacts. In-process MainWindow tests drive real widgets
with injected application services. If cross-process UI automation needs a harness,
build it separately rather than exposing an unrestricted test-control API in releases.

## Fixtures, isolation and asynchronous behavior

Extract only shared infrastructure that has multiple consumers:

- A scoped environment/settings fixture creates fresh data, settings, socket and
  artifact directories; it restores prior environment values, rather than merely
  unsetting them. Use per-test isolation for mutable settings and explicit teardown.
- A loopback HTTP/WebSocket server records requests, checks authentication, and can
  release delayed replies, fragment frames, reject requests or disconnect. Unknown
  requests fail by default instead of returning `{}`. Avoid implementing a second
  mihomo: script narrow protocol responses and compare key contracts with the real
  core. Redact credentials from failure transcripts.
- A compiled fake-core executable supports validation success/failure, readiness,
  crash and controlled exit on macOS/Windows/Linux. Define OS-specific termination
  expectations explicitly; it cannot assume Unix signal semantics on Windows.
- A fake helper uses explicit framed messages/lease state; a command runner records
  OS operations and returns declared outcomes. Share transport mechanics without
  sharing the production validator as the expected-result oracle.
- Inject browser operations, operation deadlines and scheduler/clock dependencies
  at narrow constructor boundaries. Avoid public mutable test knobs or searching
  QObject internals to force timers. Production defaults remain unchanged.

Keep real filesystem, YAML parsing, Qt networking and QProcess in their integration
tests; substitute only the boundary under test. Coordinator tests may use fake
backends, but at least one integration path must use each real adapter.

Use explicit gates for ordering: hold an operation, observe pending state and event
loop progress, release it, then await completion with a deadline. Fake time helps
scheduler/expiry tests; actual I/O still needs real asynchronous integration checks.
Avoid arbitrary sleeps and tight machine-dependent latency assertions in functional
tests. On timeout, report pending operations, state, and redacted request history.
Always terminate owned fixture children and drain workers before deleting fixtures.
Parallel jobs use ephemeral ports and unique socket/data paths; serialize tests that
truly share a native display or system resource instead of relying on lucky timing.

## UI, performance and native networking

UI tests assert behavior: navigation, loading/empty/error states, enablement,
selection, keyboard access, persistent choices, and bounded layout at compact/default
sizes. Use stable semantic object names where necessary, without making private
QObject structure the contract. Test theme variants and long labels through a small
representative matrix, not every page at every possible size.

Keep real-network and native-display validation separate from offscreen tests.
Native checks verify QQuickView embedding, menus, focus and actual graph rendering.
If image comparisons are introduced, pin fonts/style/device scale and allow small
rendering tolerances; preserve images for review. Offscreen success cannot certify
native GPU rendering. Move frame pacing out of data-pages into `benchmarks/` and
record workload, hardware/backend, warmup, duration and percentiles. Benchmark
budgets belong to controlled runners rather than ordinary developer machines.

Native proxy, TUN and helper-install workflows run only on disposable OS environments
with explicit resource ownership and cleanup checks. Cover macOS, Windows and Linux
independently. Skipped platform cases are visible gaps, not passes. Normal test runs
must not contact real subscriptions, alter the developer's VPN/proxy, install a
helper, or read personal credentials.

## Build and CI gates

| Gate | Proposed execution | Required evidence |
| --- | --- | --- |
| Every change | Architecture, core/platform fake-adapter contracts, app integration and offscreen UI on supported OS jobs | Correctness, deterministic cleanup, no unexpected skips; fresh configure/build |
| Main branch / core changes | Pinned real-core workflows and installed-artifact smoke | Actual controller/stream compatibility, local proxy traffic, relocatable QML/resources/runtime dependencies |
| Scheduled / before release | Native display, disposable privileged/network scenarios; long lifecycle sequences; sanitizers where supported | Platform-specific integration, resource leaks/races, repeated startup/recovery/shutdown |
| Dedicated performance | Native graphs and bounded large-data workloads | Comparable measurements with machine-specific budgets |

Start with the available macOS lane and mark Windows/Linux as pending until runners
are configured. Do not claim the matrix is covered because CMake can register it.
Link tests against the same production libraries as the app; use explicit test
variants only where unavoidable. Set CTest timeouts and labels; keep native and
privileged targets opt-in. Store failed logs/state, Qt/core versions, fixture seed,
and relevant images as artifacts. Avoid auto-retries that hide failures; diagnostic
reruns do not turn a failing first run into an unexplained pass.

Use coverage reports to find unexercised responsibilities, with no blanket line
percentage as the acceptance rule. For high-value contracts, periodically introduce
a deliberate local fault (wrong layer precedence, stale reply accepted, ownership
check removed) and verify the expected test fails. Add seeded sequence/property
tests for bounded history, configuration preservation, and lifecycle transitions
after deterministic examples exist; print seeds and retain minimized failures.

## Migration from the current suite

| Current source/entry | Destination and treatment |
| --- | --- |
| `runtime_test.cpp` | Split into config, profile/subscription, execution and application coordination suites; retain behavioral assertions |
| `controller_test.cpp`, `provider_test.cpp` | `core/mihomo/`; share narrow fixture transport; add WebSocket contract coverage |
| `backup_test.cpp` | Archive/WebDAV in `core/backups/`; move cross-service restore ordering to `app/`; add W04 |
| `traffic_history_test.cpp` | `core/telemetry/`; retain value tests and add seeded invariant cases only where valuable |
| `system_proxy_test.cpp` | `platform/`; replace `.cpp` inclusion/internal globals with an injected runner and observable state |
| `system_proxy_async_test.cpp` | `platform/`; retain serialization/ownership contracts and replace timing assumptions with gates |
| `privileged_service_client_test.cpp` | `platform/`; introduce deadline dependency; retain framing/authentication/lease contracts |
| `data_pages_test.cpp` | Split by feature into `ui/pages/`; move native frame measurement to `benchmarks/`; retain graph behavior tests |
| `routing_controls_test.cpp`, `tray_test.cpp` | `ui/shell/`; keep widget behavior, move shared routing decisions into `app/` contract tests |
| `dashboard_async_test.cpp` | `ui/shell/`; inject browser operations rather than replacing linker symbols |
| `privileged-helper`, `privileged-helper-ipc` | Preserve both self-test entry points and native implementation coverage; keep production/test helper builds explicit |

Implementation sequence:

1. Record a reproducible baseline, including skips and environments; create the
   feature coverage index and test labels. Keep existing assertions and CTest names
   through the source-folder move; document later renames rather than registering
   old/new names that accidentally run the same test twice.
2. Extract scoped fixtures and replace the three intrusive seams (proxy source
   inclusion, browser symbol replacement, internal timer mutation). Add portable
   fake execution and real WebSocket tests. Link newly extracted production targets.
3. Add architecture checks alongside the source refactor and broad feature contracts
   F01–F10. Prioritize ordinary profile/subscription/core flows before more isolated
   edge cases; introduce app tests with the coordinators they exercise.
4. Implement W01–W05, real-mihomo smoke and package checks. Establish native/benchmark
   lanes separately. Remove duplicate regressions only when the replacement names
   and assertions are recorded in the coverage index.

OAuth/policy, usage reporting, Tailscale/EasyConnect, plugins and a possible Windows
COM facade are future feature suites. Add their contracts with their implementations
(provider substitution, version mismatch, policy precedence, route/DNS conflicts,
identity isolation and recovery); do not add empty passing stubs now.

## Portable COM and capture qualification additions

Component tests derive requirements from the reviewed reference rather than copying
its tests blindly. In particular, validate real failure Result values (the reference
uses unsigned values with negative failure checks), invalid/null query outputs,
unknown/new IDs, exact retain/adopt/release semantics, interface identity and query
consistency, thread affinity and construction failures. When weak references are
introduced, test resolution/destruction races and control-block/library lifetime.
Architecture checks prevent FX-prefixed published names and UI links to private
backend implementation targets. Reference attribution paths remain allowed.

Test a separately compiled consumer against a shared module on each OS: ABI version
mismatch, factory failure, asynchronous callbacks, close/cancel, and attempted unload
with outstanding objects/operations. Windows uses the same portable COM model; no
native registry/marshaling tests are required unless that optional bridge is added.

Capture tests use two fixture apps and a local server to distinguish targeting from
post-capture filtering. Include HTTP/1/2 and separately qualified HTTP/3/WebSockets,
binary/compressed/multipart content, long streams, opaque app encryption, requests
and replies, and new inbound server connections as distinct cases. Assert content
semantics, not identical TLS packets. No-trust, pinned and invalid-upstream tests
must not pass by globally disabling certificate verification.

Native target selectors need PID-start-time/executable checks, explicit child-process
policy and truthful unknown attribution. Test own-worker/core exclusions, TUN/VPN
coexistence and loop prevention. Passive mode must not retain bodies or change trust;
raw packet/header parsing used for metadata is bounded. Verify monitor container
restarts and lost events rather than treating an HTTP health endpoint as capture proof.

Native flow save/reopen is the MITM oracle. HAR/JSON/CSV exports have explicit protocol
coverage and size limits; native flow and exported body data are compared independently.
Test disk-full, quota, crash fragments, backpressure, body streaming completeness and
redacted derived exports. Fake-clock schedule tests cover run identity, timezone/DST,
sleep/missed runs, overlap, app restart/unavailability, script timeout/error, and
cancel/flush/cleanup. Always-on service behavior is a separate native test lane.
Test pre-existing connections versus those opened after capture readiness, immutable
script snapshots when the source script is edited, and a bounded drain timeout with
active HTTP/WebSocket/TCP streams. Partial captures must remain explicitly partial.

Capture-related suites live under `tests/contracts/capture/`, `tests/core/capture/`,
`tests/app/capture/`, `tests/integrations/capture/`, `tests/platform/capture/`, and
`tests/ui/pages/capture/`; workflows W06–W08 and package tests have independent owners.
Routine CI uses disposable fixture credentials/trust and loopback servers. Native
tests require isolated machines/users, not actual user apps or system trust changes.
Use the local mitmproxy checkout and pinned dependencies; upstream tests follow
its AGENTS.md (`uv run pytest`/`uv run tox`). `make test-capture` exposes this lane.

Completion means every current key feature has an identifiable success contract,
important failure/recovery coverage, and appropriate cross-feature evidence; module
boundaries are executable checks; normal workflows exercise real application wiring;
and platform/real-core limitations are visible. More tests alone are not the goal.
