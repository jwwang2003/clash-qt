# TERMINATION DEADLINE FIX — proof notes

Scratch: `/tmp/clash-qt-p4.w0Y8Uo/timer-fix`. Snapshot `snap/` is a copy of
`transport-fix/snap-base` (frozen COMMITTED transport sources; the ongoing
transport `.cpp` edits are excluded) plus ONLY this lease's three files, synced
by `sync.sh`. Build `build/` (Debug, `CMAKE_PREFIX_PATH=/opt/homebrew`). No repo
build, no writes outside the three owned files and this directory.

Preferences plist `com.clash-qt.clash-qt.plist` was hashed before AND after every
run by `/tmp/clash-qt-p4.w0Y8Uo/run_check.py`; it read
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` on every one
(`logs/*`, and `check-runs.jsonl` labels `tfx-*`, `inv-M*`). Every run had an
isolated `CLASH_QT_DATA_DIR`, `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`. Only the built fake core and in-process loopback
fixtures; no privileged helper, no system network, no submodule change.

## Mechanism (installed Qt 6.11.1 headers + local measurement)

`qtimer.h:176-183`: `QTimer::defaultTypeFor()` returns `Qt::CoarseTimer` for any
interval `>= 2s`, `Qt::PreciseTimer` below ("coarse timers are worst in their
first firing"). `terminateProcess()` armed the kill with
`QTimer::singleShot(timings_.terminateWaitMs /*3000*/, child, ...)` — no timer
type — so the escalation ran on a coarse timer that may fire EARLY. The 400 ms
the W05 case used to push in through `setTimings()` was under 2 s and therefore
precise, which is exactly why the defect stayed invisible.

`demo/timer_probe.cpp` (standalone, Qt6::Core only), `logs/demo-*.txt`:

| interval | implicit (shipped) earliest | explicit PreciseTimer | deadline-armed |
|---|---|---|---|
| 3000 ms | **2851 ms** (early in 6/6 rounds) | 3000 | 3000 |
| 2000 ms | 1952 ms | 2000 | 2000 |
| 400 ms | 400 ms (never early) | 400 | 400 |

That reproduces the reported W05 measurement (2962 ms of a published 3000 ms).

## Change

`CoreProcess::terminateProcess()` only (plus a `<QDeadlineTimer>` include):
a `QDeadlineTimer grace(timings_.terminateWaitMs, Qt::PreciseTimer)` captured by
value, and a self-re-arming `QTimer::singleShot(..., Qt::PreciseTimer, child, ...)`
that re-checks `grace.remainingTime()` and re-arms while any remains, killing
only once the monotonic deadline is spent. `child` stays the context object. No
header, build, facade or knob change; no new member. The owned native-work pool
(`parsePool_`/`parseWork_`/`pendingNativeWork`) is untouched.

Tests: new `CoreProcessTest::aChildRefusingToTerminateKeepsItsWholePublished
GraceBeforeTheKill()` in the EXISTING core-process suite — a POSIX child that
installs `SIG_IGN` for SIGTERM and only then prints `refusal-armed`; the case
waits for that line, stops, and asserts the stop took `>= process.timings()
.terminateWaitMs` (the SHIPPED 3000 ms, no `setTimings()`; under 2 s the call
would pick a precise timer anyway and cover nothing) and `< published + 5000`.
W05 gains a precondition: it waits for the `[INFO] stubborn` marker (the fake
core installs the refusal before printing it, `fake_core_main.cpp:172-175` ahead
of the stdout directives) before starting the measurement. No new ctest
registration — both cases join registered suites.

## Evidence

Fixed snapshot: `core-process` 9/9 passed x3 runs (`logs/cp-fix-*.txt`);
`w05-recovery` full suite 6/6 passed x2; the refusal case alone 8/8 passed
(~3.34 s each, `logs/w05-*.txt`); `backend-real-contract` 95/95,
`backend-contract` 41/41, `backend-bridge` 27/27 (`logs/backend-suites.txt`).

External mutations, applied to `snap/` only, rebuilt and run there:

* **M1** — actual wait shortened by 300 ms, published `terminateWaitMs` left at
  3000. core-process regression FAILED ("killed 2753 ms into its published
  3000 ms grace"); W05 FAILED ("quit finished in 2743 ms"). `logs/inv-M1-*`.
* **M2** — the shipped defect restored verbatim (implicit coarse `singleShot`).
  core-process regression FAILED 2 of 3 runs at **2961 ms** and **2902 ms** —
  the same signature as the reported 2962 ms — and passed once. The defect is
  probabilistic by nature, so this case detects it probabilistically; M1's
  deterministic shortening is what it catches every time. `logs/inv-M2-*`.
* **M3** — instrumented probe of the fixture race: at the moment `Running` was
  observed, the `[INFO] stubborn` marker had ALREADY arrived in 3/3 runs
  (readiness at 197-203 ms). The race does NOT reproduce on this machine; the
  new W05 wait is therefore an asserted precondition, not a demonstrated fix.
  `logs/inv-M3-race.txt`.
* **M4** — the marker removed from the fixture script: the new W05 guard FAILED
  with "the child never announced its refusal, so the termination bound would
  measure a polite exit", i.e. the guard is live rather than vacuous.
  `logs/inv-M4-guard.txt`.
