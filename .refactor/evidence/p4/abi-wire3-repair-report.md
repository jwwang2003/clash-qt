# Final ABI audit repair — registrations, proofs and open gates

Opus worker. Repository writes confined to the leased paths. No agents spawned,
no git, no build files, no `build/` or `build/dev` use. All compilation and all
runs in `/tmp/clash-qt-p4.w0Y8Uo/abi-final-fix/**`.

Toolchain: Darwin 25.6.0 arm64, AppleClang 17.0.0.17000603, Qt 6.11.1,
yaml-cpp 0.9 (`-Dyaml-cpp_DIR=/opt/homebrew/lib/cmake/yaml-cpp`; the default
pick resolved anaconda's 0.8 and failed to link — recorded because the
coordinator's own configure will need it or will need the anaconda copy off the
path).

Engine: `abi-final-fix/mihomo`, sha256
`6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544`, the
source-built copy the auditor used (`3rdparty/mihomo` @ `ab405bad`, v1.19.31).
Fixtures bind `127.0.0.1:29351/29352/29353`, `mixed-port: 0`, `tun.enable:
false`. No helper, no system proxy, no VPN, no trust store, no subscription, no
Clash Verge discovery, no submodule touched. The installed Clash Verge process
seen in `pgrep` was pre-existing and never touched; after every engine-driving
run `pgrep -f abi-final-fix/mihomo` was empty and :29351 was free.

Isolation: every process through `../run_check.py` — fresh `CLASH_QT_DATA_DIR`,
`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, and
`com.clash-qt.clash-qt.plist` hashed before and after. 104 runs under labels
`fx-*`, `fxinv-*`, `final-*` in `../check-runs.jsonl`; every one reads
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` before AND
after, and the file is still that now.

---

## Baseline: all three NO-GO defects reproduced first

| Defect | Run | Outcome |
|---|---|---|
| cross-thread final Release | `fx-base-cross-thread` | exit **-11** (SIGSEGV), `QSocketNotifier: ... from another thread`, `live objects after cross-thread release: 0` |
| retained-root retry | `fx-base-retained` | exit 1, `retry unload()=0 lastError=-9 'no module is loaded'` |
| QDateTime representation | `fx-base-datetime` | exit 1, 5 failures, `spec 1->0 offset 0->28800` |

---

## F1 — final Release from any thread, and native async work

`src/core/mihomo/module/backend_session.{h,cpp}`,
`src/core/mihomo/process/core_process.{h,cpp}`,
`src/core/mihomo/mihomo_backend.h`, `src/core/mihomo/module/mihomo_module_entry.cpp`,
`src/integrations/component/module_backend.{h,cpp}`, `src/core/component/abi/wire.h`.

* `BackendSession::Release()` → `retire()`. Same thread as the host install, or
  no host yet: `delete this`. Any other thread: a `QEvent` is posted to
  `OwnerThreadRetire`, a plain `QObject` (no `Q_OBJECT`, so no moc and no build
  registration change) created in `SetHost` with the owner thread's affinity.
  The event type comes from `QEvent::registerEventType()`, not a hard-coded
  `User + n`.
* The refcount promise is NOT narrowed: `Release()` still returns 0 when the
  caller's reference is gone. What is deferred is the DESTRUCTION, and the
  object — and therefore the module's live-object count, and therefore the
  mapping — survives until the queued teardown has run. Measured: live count is
  **1** immediately after the cross-thread release and `unload()` refuses.
* `ModuleCount` is the first-declared member, so it is destroyed LAST; the
  module's count is no longer released in the destructor body.
* `WrappedBackend::pendingNativeWork` (new `std::function<int()>`), supplied by
  `makeRealBackend` from `MihomoBackendImpl::pendingNativeWork()`; the fake
  module supplies none and reports 0.
* `CoreProcess` owns `parsePool_` (`QThreadPool`, max 1 thread) instead of the
  global pool, tracks submitted-and-not-exited runnables in a shared atomic
  claimed BEFORE submission and released by a guard INSIDE the runnable, has a
  shared cancel flag checked at the runnable's entry and mid-way, and
  `~CoreProcess` cancels then `waitForDone()`s before anything else.
* `OutstandingWork()` adds the native term; `Close(timeout)` drains against it
  and its diagnostic COUNTS the survivors (`"... and 1 native task(s) were
  still outstanding at close"`).

New API: `abi::kCmdPendingNativeWork = 0x0602` (additive),
`ModuleBackend::pendingNativeWork()` (**-1** when the module is too old to
answer, deliberately distinct from 0), `MihomoBackendImpl::pendingNativeWork()`,
`CoreProcess::pendingNativeWork()`, `CoreProcess::cancelServiceParse()`.

Proof: `final-*-fix-cross-thread-final-release` (16 assertions, Debug and
Release, 0 failures) and `final-*-fix-service-parse-cancel`. The parse case
drives the REAL engine in service mode against a fake injected host seam — no
helper exists anywhere in it — with a 5.6 MB valid config; native work is
observed at 1 while queued/running, `OutstandingWork()` is 2, `Close(…, 0)`
answers `kTimeout` (-6) and names the surviving task, and by the time it
returns the runnable has exited and the image unmaps.

## F2 — retained root: recoverable refusal and a working retry

`src/core/mihomo/module/backend_module.{h,cpp}`,
`src/integrations/component/module_loader.{h,cpp}`.

The coordinator's protocol, implemented and documented on `ModuleLoader::unload`:

1. release the ROOT interface, keep the `IModuleLifetime` one — the object stays
   pinned, so the question is answerable;
2. `PrepareUnload()` requires zero live objects AND `references_.Value() == 1`,
   and refuses with `kInvalidState` **without latching**;
3. on refusal the loader restores the root via
   `QueryInterface(kIComponentModuleId)` (new private `restoreRoot()`) and
   returns **false**; nothing changed, `isLoaded()` is true, `createSession()`
   still works;
4. on success the last reference is dropped and the image unmapped.

`unload()` now returns false on a refusal rather than true-with-a-leak. The
`kFalse` "prepared but the runtime could not be pinned" path is unchanged.
Component-r1 vtables are untouched; no new IID was needed.

Proof: `final-*-fix-retained-root-protocol`, 17 assertions, Debug and Release,
0 failures, including a second refusal behaving like the first and a session
created between the two.

## F3 — timestamps keep their representation; wire revision 3

`src/core/component/abi/wire.h`,
`src/integrations/component/marshal/backend_marshal.{h,cpp}`,
`src/integrations/component/marshal/connection_snapshot.cpp`.

* General codec: i64 instant, u8 `TimeRepresentation`, i32 offset seconds, UTF-8
  zone identifier. New exported names: `abi::TimeRepresentation`
  (`kTimeLocal/kTimeUtc/kTimeOffsetFromUtc/kTimeNamedZone`),
  `abi::kTimeRepresentationMax`, `abi::kMaxUtcOffsetSeconds`;
  `marshal::timeRepresentationOf`, `marshal::timeZoneIdOf`,
  `marshal::dateTimeFromParts`.
* Packed snapshot: `ConnectionRecord` grows to **184** bytes with `startZone`,
  `endZone` (TextRefs into the SAME interned blob), `startOffsetSeconds`,
  `endOffsetSeconds`, `startSpec`, `endSpec` and an explicit 6-byte reserved
  tail. Still one owned buffer, still fixed-layout offset records. The TextRef
  run is 14 and the `static_assert`s were updated with it.
* Validation: unknown spec, offset outside ±16 h, a named zone with no payload
  or one this build cannot resolve — each refused by name; the general codec
  latches the reader so the command is refused as `kInvalidArgument`.
* `kWireRevision` 2 → **3**, `kConnectionSnapshotVersion` 1 → **2**. The runtime
  tag hashes the wire revision, so an older module is refused at the handshake —
  proven still working by `audit-probe --mode handshake` (`a stale wire revision
  is refused`).
* `kMinConnectionBytes` / `kMinProviderBytes` grew by
  `2 * kTimestampRepresentationBytes`.
* No facade or Qt-bridge change. `connections_page.cpp` is untouched and now
  renders exactly what the backend produced.

Proof: `audit-probe --mode datetime` 0 failures (was 5), Debug and Release;
`aTimestampKeepsItsRepresentationAndNotOnlyItsInstant` (UTC, +05:45, −09:30,
Asia/Tokyo, America/St_Johns, local, invalid — instant, `toString(Qt::ISODate)`,
`timeSpec()`, `offsetFromUtc()` and zone id each asserted);
`aTimestampWithAnUnusableRepresentationIsRefused`;
`everyTimeRepresentationSurvivesAtSnapshotScale` (500 rows, mixed
representations, and the shared zone identifier costing < 128 bytes across all
of them).

## F4 — a refused dispatch names itself

`src/core/mihomo/module/backend_session.cpp`. `invokeImpl` clears the
diagnostic on ENTRY, fills one in for any dispatch failure that did not set a
more specific one, and answers the test-control range with a diagnostic instead
of a bare `kNotImplemented`.

Proof: `everyRefusedCommandLeavesItsOwnDiagnostic` — five commands, a FRESH
session each, the pre-state asserted as `kNotFound`, the code and the command
number asserted in the message, a SECOND failure of a DIFFERENT command on the
same session asserted to name the second command, and a later success asserted
to clear it. `fix-probe --mode dispatch-diagnostic` repeats it against the
SHIPPING module.

---

## Inversions — external copy only, 14 of 15 detected

`probe/fix_inversions.py`, results in `notes/inversions.json`. Pristine copy
restored before and after every case and verified byte-identical at the end
(`snapshot restored byte-identical: True`).

| Inversion | Detector | Outcome |
|---|---|---|
| final Release destroys on the calling thread | fix-probe cross-thread | **exit 245 (crash)** |
| module count released in the destructor body | fix-probe cross-thread | SURVIVED — see below |
| `OutstandingWork()` omits native work | fix-probe service-parse | exit 1 |
| `Close` does not drain native work | fix-probe service-parse | exit 1 |
| parse on the GLOBAL pool, no `waitForDone` | fix-probe service-parse | **exit 245 (crash)** |
| `PrepareUnload` ignores other holders | fix-probe retained-root | exit 1 |
| loader drops every reference before asking | fix-probe retained-root | exit 1 |
| timestamps carry only their instant | abi-contract-tests | exit 1 |
| packed record drops the zone | snapshot-marshal-tests | exit 1 |
| an unresolvable zone degrades to local time | abi-contract-tests | exit 1 |
| packed validator accepts any representation | abi-contract-tests | SURVIVED — redundancy |
| …and the decoder's check removed TOO | abi-contract-tests | exit 1 |
| a refused dispatch leaves no diagnostic | abi-contract-tests | exit 1 |
| `Invoke` keeps the previous call's diagnostic | abi-contract-tests | exit 1 |

Two notes on the survivors, neither dressed up:

* **`packed-validator-accepts-any-representation` is redundancy, not a gap.**
  The packed validator's range check and `dateTimeFromParts`'s own refusal each
  catch it alone. Removing BOTH is detected (row 12), which is what a real gap
  would look like. Same category as the auditor's known-surviving
  `module_backend.cpp:418` mutant.
* **`module-count-released-in-the-destructor-body` is UNVERIFIED by inversion,
  and is reported as such.** The ordering is required by the parent review and
  is implemented, but with this class's current member set the observable window
  is empty: everything slow (`wrapped_`, `privileged_`, the host reference) is
  already torn down by `Close()` before the destructor body runs, and the
  members destroyed after it (`retire_`, a `QSet`, two `QString`s, a
  `std::vector`) take no measurable time, so no single-threaded observer can
  read the count inside the window. The guarantee is defence for members added
  later. I did not manufacture a flaky two-thread poller to claim a proof.

A harness defect worth recording: `shutil.copytree` preserves mtimes, so the
first inversion run left ninja thinking restored sources were up to date and
mutants accumulated across cases. The run above `os.utime`s the tree after every
restore; the numbers here are from after that fix, on a wiped build directory.

---

## Prior hardening preserved

All 16 proofs from the earlier rounds were re-run and hold: activated real
engine reaching ready + CONFIRMED stop with the component absent from the dyld
list and from `RTLD_NOLOAD` while QtCore/QtNetwork stay mapped; the three
anchored buffers counted at exactly 3; handshake size validation with canaries;
observer admission by production; containment at COM entry points; packed-buffer
bounds; no helper/platform symbols in the module; the independent standalone
`component_consumer`; and host exit 0.

The create/unload race proof is preserved but had to MOVE: `audit-probe --mode
concurrency` races a caller that still holds its root reference, which
`PrepareUnload` now refuses for a reason unrelated to the race, so its
"quiescent sometimes" assertion is vacuous against the new rule. The same
experiment under the protocol is `fix-probe --mode concurrency-protocol`:
**3000 rounds, 2849 creations, 2856 quiescent answers, 144 genuinely lost
races, 0 objects created after the latch.** The repository's own
`creatingAndPreparingInParallelNeverHandsOutAnObjectAfterPrepare` was moved to a
raw `entry()` for the same reason and still passes.

## Auditor probes that now fail BY DESIGN

`audit_probe.cpp` is kept byte-identical and is still run. Two modes carry
assertions that encode the design the coordinator replaced:

* `retained-root`, 3 failures: `unload()` now returns **false** on a refusal
  (was true-with-a-leak), `lastErrorCode()` is `kInvalidState` (was `kFalse`),
  and `retained->Release()` returns 2 rather than 0 because the loader
  deliberately still pins the root — that is precisely what makes the retry
  possible. Its last two assertions, the NO-GO ones — "RETRY of unload()
  succeeds" and "image is UNMAPPED after release-and-retry" — now **pass**.
* `concurrency`, 1 failure: as explained above.

Everything else in `audit_probe.cpp` passes unchanged in both configurations.

## Not done / open

* Windows and Linux are **not compiled and not qualified**. No cross toolchain
  exists here. The new header code (`TimeRepresentation`, the record fields) is
  plain fixed-width C++ with no platform branch, but that is a reading, not a
  measurement.
* The coordinator's removal of the universal packed ≤ naive timing assertion and
  the rename to `comparePackedAndNaiveAtSnapshotScale` are preserved untouched.
  The new timestamp cases are correctness cases in the ordinary lane.
* `tests/contracts/component/abi/CMakeLists.txt` and the repository build files
  are coordinator-owned and were NOT edited. No new build registration is
  needed: every new case lives in files already registered.
* Qualification is macOS arm64 / AppleClang 17 / Qt 6.11.1 only.
