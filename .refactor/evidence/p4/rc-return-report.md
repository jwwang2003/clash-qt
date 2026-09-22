# RC package — Release return semantics: registrations, proofs, open items

Opus worker. Repository writes confined to the four leased files. No agents, no
git, no build files, no repository build. All compilation and every run in
`/tmp/clash-qt-p4.w0Y8Uo/rc-fix/**`.

## Inputs and isolation

* Snapshot `snap/` = working tree of `src/` and `tests/` copied once (23:0x), plus
  `snap-pristine/` (same copy, never edited) as the pre-fix comparison build.
  Standalone `snap/CMakeLists.txt` reused from `abi-final-fix`. Transport-worker
  files (mihomo_client/provider_client/mihomo_backend/fake_backend) are copies;
  none were written.
* Builds: `build` (Debug), `build-rel` (Release), `build-base` (Debug, pristine
  product + same probes). `yaml-cpp_DIR=/opt/homebrew/lib/cmake/yaml-cpp`.
  Qt 6.11.1, AppleClang, Darwin 25.6.0 arm64.
* Engine `rc-fix/mihomo`, sha256 `6f53b2e1…5544` (the source-built copy prior ABI
  workers used). Fixtures bind `127.0.0.1:29351`, `mixed-port: 0`, `tun.enable:
  false`, no privileged helper, no system proxy/VPN/trust store, no subscription,
  no Clash Verge discovery, no submodule touched. The installed Clash Verge
  engine seen in `pgrep` was pre-existing and never contacted. One orphaned
  scratch engine (left by a deliberately crashing inversion run) was terminated;
  `:29351` verified free afterwards.
* Every process through `../run_check.py`: fresh `CLASH_QT_DATA_DIR`,
  `QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, prefs hashed before
  AND after. 103 runs labelled `rc-*` in `../check-runs.jsonl`; all 206 hashes
  read `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`.

## The change

`BackendSession::Release()` no longer returns component-r1's zero for an object
it has not destroyed.

* `mustDeferFinalCleanup()` — true when the owner thread is not the calling
  thread, or when it IS but `nativeWorkCount() > 0`. False for an inert session
  (no host ever installed), for a quiescent one on its own owner thread, and
  when there is no `QCoreApplication` left to defer to.
* Not deferring: `delete this` inside `Release`, then `return 0`. Earned.
* Deferring: `references_.Increment()` — a real cleanup reference — then
  `scheduleOwnerThreadCleanup()`, then `return held` (1). The decrement to zero
  happened first, so this thread owns the object outright and no other holder
  can observe the count between the two.
* `runOwnerThreadCleanup()` on the owner thread: if a module-owned runnable is
  still in flight it re-arms (`OwnerThreadRetire::rearm()`, `startTimer(5ms)`,
  falling back to a re-post) and keeps the reference; otherwise
  `dropCleanupReference()` decrements to zero and deletes — on the owner thread.
* `scheduleOwnerThreadCleanup()` is try//catch: on allocation failure the object
  is left alive and COUNTED rather than destroyed on a thread that may not
  destroy it.
* `ModuleCount` is still the first-declared member (destroyed last); member
  cleanup order, QueryInterface/ResolveInterface, `Close` and `OutstandingWork`
  are untouched. No vtable, IID, command code or build registration changed.
  `backend_abi.h` is comments only.

New private API: `mustDeferFinalCleanup`, `scheduleOwnerThreadCleanup`,
`runOwnerThreadCleanup`, `dropCleanupReference`, `OwnerThreadRetire::rearm`
(replacing `retire`/`retireOnOwnerThread`). Nothing outside the class referenced
the old names.

## Suites and probes

`abi-contract-tests`: 40 → **45** cases, Debug and Release, 0 failures.
Corrected: `theFinalReleaseFromAnotherThreadKeepsTheModuleCountedUntilCleanupRuns`
(asserted `== 0`, now `> 0`). Added:

* `aQuiescentSessionIsDestroyedInsideReleaseAndOnlyThenReportsZero`
* `aQueuedCleanupKeepsItsOwnReferenceUntilItHasActuallyDestroyed`
* `closingAloneDoesNotConsumeTheCallersReference`
* `extraAndForeignReleasesNeverDestroyWhileAHolderRemains`
* `queryingASessionInterfaceTakesExactlyOneReference`

plus a `LifetimeHost` double that records when it was destroyed, so "a callback
arriving now has a live host" is asserted rather than assumed.

`probe/rc_probe.cpp` (new target `rc-probe`, scratch only; `audit_probe.cpp` and
`fix_probe.cpp` kept byte-identical and still run): `cross-thread-return`,
`owner-thread-native-work` (shipping module, real engine, service mode, fake
seam), `closed-session-zero`. 0 failures, Debug and Release.

| probe | pristine build | fixed build |
|---|---|---|
| audit `cross-thread-release` | 0 | **1** — `the final Release from another thread returned 0` |
| audit `retained-root` | 3 | 3 (pre-existing: the auditor's superseded kFalse protocol) |
| audit `concurrency` | 1 | 1 (pre-existing: probe holds the root, PrepareUnload refuses) |
| audit real-engine/buffers/handshake/datetime/marshalling/containment/observers | 0 | 0 |
| fix `cross-thread-final-release` | 0 | **1** — same superseded assertion |
| fix `retained-root-protocol` / `concurrency-protocol` / `dispatch-diagnostic` / `service-parse-cancel` | 0 | 0 |
| rc `cross-thread-return` | **1** | 0 |
| rc `owner-thread-native-work` | **6** | 0 |
| rc `closed-session-zero` | 0 | 0 |

The two remaining `1`s on the fixed build are the two places that spelled the
defect out: `check(remaining == 0, "the final Release from another thread
returned 0")`. Both probes' other assertions — live count 1, unload refused,
image kept, then count 0, unload, component absent, QtCore retained — still pass.
The actual-unmap probe and the pending-service-parse cancellation are unchanged.

Pre-fix behaviour recorded on `build-base`: cross-thread final Release returned
0; owner-thread final Release with a parse in flight returned 0, destroyed
synchronously (blocking in `~CoreProcess`) and let `unload()` succeed while the
probe still expected a live object.

## Inversions (external copy only, `notes/invert.py`, `notes/inversions.json`)

| inversion | detected by |
|---|---|
| `early-zero` — defer but `return remaining` (0) | 4 contract cases (`remaining.load() > 0` FALSE) + rc `cross-thread-return` + rc `owner-thread-native-work` |
| `premature-count` — module count released when the cleanup is queued | 3 contract cases + both real-engine rc cases (`still counted`, `unload REFUSED`, `image kept`), probes then SIGSEGV on the unmapped live object |
| `native-work-ignored` — `mustDeferFinalCleanup` stops checking the pool | rc `owner-thread-native-work`, 6 failures |
| `close-consumes-reference` — `closeImpl` decrements | `abi-contract` SIGSEGV (exit 245) + rc `closed-session-zero` |

Each was applied to `snap/`, rebuilt, run, and the snapshot restored from the
repository copies; `snap` and the repository files were diffed byte-identical
afterwards and the suite re-run green.

## Not claimed

* No final-gate claim. The two superseded auditor/repair assertions above are
  reported, not edited in their owners' files.
* Native-work deferral is proved with the shipping module only: the test double
  supplies no `pendingNativeWork` accessor and `fake_module_entry.cpp` is not in
  this lease.
* If the owner thread's event loop never runs again, a deferred cleanup never
  completes and the session stays alive and counted. That is the pre-existing
  shape of the deferral, now reported honestly by the count instead of hidden
  behind a zero.
