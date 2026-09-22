# W02 SETTLE FIX — evidence, gates, inversions

Package: FRESH-CLONE W02 SETTLE FIX. Writable repo path used: exactly one,
`tests/workflows/w02_subscription_update_test.cpp`. No production, build,
CMake, preset, main.cpp, architecture.json or records write. `fresh-2` was
read only; never written, never rebuilt.

## Environment

* External copy: `git clone --no-hardlinks` of the working repo at `ff42815`
  into `/tmp/clash-qt-p4.w0Y8Uo/w02-settle/src`. Its
  `w02_subscription_update_test.cpp` is byte-identical to fresh-2's (verified
  with `diff`).
* Standalone build: `cmake --preset dev -B .../w02-settle/src/build/p4`,
  targets `w02-subscription-update-tests`, `clash_qt_backend_module`,
  `clash-qt-fake-core`. Nothing built in the repository, in `build/dev`, or in
  `fresh-2`. Submodules untouched; the engine was NOT rebuilt.
* Engine under test: the FROZEN fresh-2 artifacts, by explicit path —
  `CLASH_QT_CORE_BINARY=/tmp/clash-qt-p4.w0Y8Uo/fresh-2/build/p4/core/mihomo`,
  `CLASH_QT_CORE_PROVENANCE=.../core/mihomo-provenance.json`
  (source_commit `ab405bad5bee…`, v1.19.31, clean, sha256 `6f53b2e18687…`).
* Module under test: this scratch build's
  `build/p4/modules/libclash_qt_backend_module.dylib`.
* Every run through `run_check.py`: isolated `CLASH_QT_DATA_DIR`,
  `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, native plist hashed
  before and after. 108 runs; `before` and `after` are a single value across all
  of them: `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`.
* TUN disabled by the generated configuration, inbound on a port the case
  reserves, external-ui directory seeded (the existing guard, unchanged), local
  loopback subscription fixture only. No helper socket, no app binary, no system
  proxy/VPN/trust-store/network change, no real subscription, no Verge
  discovery. Harnesses: `run-real.sh`, `run-fixture.sh`, `run-bin.sh`.

## Initial cause — measured, not assumed

`RuntimeCoordinator::onRuntimeConfigReady` calls `backend_.start()` and only
then emits `coreStartRequested` (runtime_coordinator.cpp:87-88). The host
answers a start by validating the candidate in a SEPARATE child while the
outgoing core keeps running (lifecycle.h:76-85, core_process.cpp:284-312), and
consumes the held launch only once the retiring child's exit is observed
(core_process.cpp:504-509, 544-550).

So at the ORIGINAL assertion point — `launched.size() >= 2` then
`state() == Running` — the host is still describing the OUTGOING engine.
Instrumented runs (`logs/probe-*.log`, `logs/sweep-*.log`, `logs/hunt-*.log`)
report at that exact instant, on every run:

    state=2 (Running)  connected=1  generation UNCHANGED  readyEndpoints=1
    isRestartPending()=1  activeConfigPaths() = 2 paths

`state==Running` rules out `terminateProcess()` having run (it sets Stopping,
core_process.cpp:540), so `pendingLaunch_` is empty and `isRestartPending()`
is `validation_ != nullptr`: a validation child is in flight. That child plus
the still-running outgoing core is the `liveProcesses 2` fresh-2 recorded.

Direct reproduction of the fresh-2 failure, in this external copy, with the
ORIGINAL test binary, under an added CPU load burst (`logs/load-orig-*.log`):

    ORIGINAL under load: pass=4 fail=2
    FAIL!  : theRealSourceBuiltCoreRefreshesAndRejectsAnInvalidUpdate()
       Actual   (wf::liveProcessesOf(binary)): 2
       Expected (1)                          : 1
       Loc: [.../w02_subscription_update_test.cpp(940)]

Identical assertion, identical message, identical line as
`fresh-2-logs/make-test-integration.log`. Unloaded the same binary passed
11/11 — the count was never the defect, its POSITION was.

NOT DIRECTLY OBSERVED: I could not catch the validator in an external `ps` or
`pgrep` sample of my own; its life is a few tens of milliseconds and the best
sampling period I achieved was ~25 ms (macOS `pgrep` has no `-a`, and `ps -A`
on this host costs ~400 ms/iteration). The two-process attribution is the
production invariant above plus the host's own reported state, plus fresh-2's
recorded `2`. Reported as derived, not as a direct sighting.

Second, quieter half of the same defect: when the original assertion PASSED it
counted the outgoing core. Inversion 2 below shows that directly.

## The fix

Ordering plus correlation, no weakening, no sleep, no expected-failure, no skip.
The process-count and applied-configuration assertions now run AFTER a
replacement gate, and the gate is stated in the host's own published terms.

New test-local API (all inside the one writable file):

* `ReadyLedger` (anonymous namespace, a `cb::BackendObserver` registered on
  `app.backend` for the life of the case) — records every `coreReady` WITH the
  `Completion::generation` it carries. `wf::JourneyObserver` keeps only the
  endpoint. `coreReady` is "the managed core answered GET /version", and as the
  terminal outcome of an operation that bumped the generation it carries the
  POST-bump value (backend-r2 A1; `startGeneration_ = generation_` after
  `bumpGeneration()`, mihomo_backend.cpp:836-840). A readiness stamped strictly
  newer than the generation the refresh replaced can only be the incoming
  engine. Members: `readyAfter(Generation)`, `count()`, `transcript()`.
* `replacementGap(app, ready, replaced, expected)` — which clause is still
  open, or empty:
  1. a readiness stamped newer than `replaced` exists;
  2. it announced `wf::kGeneratedControllerPort`;
  3. `ModuleBackend::isRestartPending()` is false — no validation child, no held
     launch, no configuration parse (lifecycle.h:133, core_process.cpp:585);
  4. `ModuleBackend::activeConfigPaths()` equals EXACTLY `{expected}` — a
     retiring child's configuration, a validation candidate and a held launch
     all appear there (core_process.cpp:587-595), so "exactly this one" says
     none of them is left (contract 5.1).
* `awaitReplacement(...)` — the same quiet window, liveness probe
  (`refreshVersion()`) and Running/attached conditions as the existing
  `awaitSettledGeneration()` (kept, still used for the initial settle), PLUS
  `replacementGap()` empty and the generation strictly past `replaced`. Carries
  the last open clause out so a budget expiry names what never happened.

Assertion order in step 4 is now: wait `launched.size() >= 2` → `awaitReplacement`
→ generation advanced → `state() == Running` → refreshed bytes → `mixed-port`
→ `activeConfigPaths().contains(launched.last())` → the retired configuration
is NOT active → `liveProcessesOf(binary) == 1` → `ready.count() == 2`.

Preserved unchanged: pinned commit/describe/sha256/size/`source_state`/toolchain
checks, the `-v` read-back, the 29097 gate, the external-UI seed and its
"UI already exists" / "External UI downloading" log assertions, the RESTful-API
bind assertion, the overrides, the invalid-update block (cached bytes,
`launched.size()`, state, generation, `activeConfigPaths`, one process, no core
failure), the confirmed quit, `awaitNoLiveProcess`, empty snapshots, zero
unknown module events and the single-Restore proxy assertion.

## Results

| run | arm | result |
|---|---|---|
| `fix-real-1..5` | real core, idle | 5/5 pass |
| `load-fix-1..8` | real core, under load | 8/8 pass |
| `fix-fixture-1..3` | fixture, idle | 3/3 pass (6 tests each) |
| `load-fixture-4..5` | fixture, under load | 2/2 pass |
| `load-orig-1..6` | real core, under load, ORIGINAL | 4 pass / 2 FAIL at line 940 |

## Inversions (external copy, rebuilt each time)

* **INV-1 — original ordering restored** (count before the gate), idle:
  10/10 PASS. Reported as *not* a failing inversion: the defect is a race that
  does not reproduce unloaded. Under load the same ordering fails — see
  `load-orig-*` above.
* **INV-2 — the gate's own predicate asked at the ORIGINAL position**:
  3/3 FAIL, deterministic (`logs/inv2-*.log`):

      INVERSION 2: at the original assertion point the handoff is not done:
      no engine newer than generation 2 has answered GET /version, so this
      process is still talking to the one the refresh replaces
      (readiness: ready(generation=1,port=29097)) | live=1 | gen=3 | state=2
      | connected=1

  This is the proof that the original position precedes the replacement
  entirely, and that its `live=1` was the OUTGOING core.
* **INV-3 — retention clause pointed at the RETIRED configuration**
  (`awaitReplacement(..., live, ...)`): FAIL, deterministic
  (`logs/inv3-1.log`): "the host still has to keep [.runtime-8d48b1db…]; the
  refreshed configuration alone is .runtime-5d489ce7…", with the ledger showing
  `ready(generation=1,port=29097), ready(generation=3,port=29097)`. The clause
  discriminates old from new; it is not vacuous.
* **INV-4 — readiness correlation dropped** (`readyAfter(Generation::Initial)`,
  i.e. any engine's readiness accepted): 3/3 PASS. NOT proven necessary by
  inversion on this host; the remaining clauses already hold the line. It is
  what makes the claim explicit and is asserted directly by
  `QCOMPARE(ready.count(), 2)`. Stated as unproven rather than claimed.
* **INV-5 — `isRestartPending()` and `activeConfigPaths()` clauses dropped**:
  3/3 PASS. Same honest caveat: the settle alone suffices unloaded. These two
  clauses are what keep the gate correct under load, where the validator and the
  retiring child are the processes the count would otherwise trip over.

Net: the load-bearing correction is the ORDERING, proved by INV-2
(deterministic) and by the loaded original-vs-fixed comparison. The retention
clause is proved discriminating by INV-3. Two clauses are not independently
proven by inversion and are reported as such.

## Build registrations

None changed, and none needed. The CTest-selected function name is unchanged:
`w02-real-core` still selects
`theRealSourceBuiltCoreRefreshesAndRejectsAnInvalidUpdate`, and
`w02-subscription-update` still selects the same four fixture functions. No new
target, no new test, no new environment variable.

Production API relied on (all pre-existing, none added):
`clashqt::integration::ModuleBackend::isRestartPending()`,
`::activeConfigPaths()`, `::refreshVersion()`, `::drain()`, `::generation()`,
`::state()`, `::isConnected()`, `::addObserver()`/`::removeObserver()`,
`core::backend::BackendObserver::coreReady(const Completion &, const Endpoint &)`.

## Artifacts

`logs/` holds every run named above; `notes/w02_original.cpp` and
`notes/w02_fixed.cpp` are the before/after bytes; `notes/w02-original-tests` is
the original binary used for the loaded comparison.
