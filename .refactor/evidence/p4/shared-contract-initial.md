# SHARED BACKEND CONTRACTS — registrations, proofs and open gates

Worker: claude-opus-5 (explicit Opus). Lease: `tests/core/mihomo/backend_real_contract_test.cpp`,
new `tests/contracts/backend/common/**`, new `tests/support/backend/common_contract_driver.{h,cpp}`.
No other file was written. `tests/support/backend/fake_backend.{h,cpp}` and
`tests/core/mihomo/CMakeLists.txt` show as modified in `git status`; **neither was touched by
this lease** (COMPONENT-ABI and the coordinator respectively).

## 1. What was built

One shared, parameterised contract set that reaches its subject only through
`core::backend::MihomoBackend &`, run as QTest data rows against four implementations:

| row | subject |
|---|---|
| `direct-fake` | `testsupport::backend::FakeBackend`, in process |
| `direct-real` | `core::MihomoBackendImpl`, real child processes, loopback controllers |
| `module-real` | `ModuleBackend` over the **shipping** `libclash_qt_backend_module.dylib`, loaded by the production `ModuleLoader` |
| `module-fake` | `ModuleBackend` over `libclash_qt_fake_backend_module.dylib` |

Both module rows really load a shared artifact through `ModuleFixture` +
`ModuleLoader::load(expectedModuleId)`; neither wraps an in-process implementation. Absent
artifact ⇒ the row `QSKIP`s by name (`unavailableReason()`), never passes silently.

### API names

`testsupport::backend::common`: `Subject{DirectFake,DirectReal,ModuleReal,ModuleFake}`,
`subjectName`, `addSubjectRows`, `currentSubject`, `Ability{DistinctControllerPayloads,
ManagedCore,ValidationOutcome}`, `Channel{Version,Providers,TunChange}`, `ContractObserver`
(records + `find/countOf/countAtOrAfter/digest`, re-entrancy witness, self-removing dtor),
`BackendDriver` (`prepare/teardown/backend/deliver/isInsideMutatingCall/waitUntil/controller/
stageVersion/stageSnapshot/hold/release/prepareValidation/startManagedCore/configPath/
settleValidation/driveToRunning/settleStop/stageTunReadback/publishHelperCoreRunning/describe/
environmentReport/unavailableReason`), `makeDriver`.
`testsupport::backend::common::cases`: the 15 scenario functions listed in
`backend_common_cases.h`.

15 shared cases × 4 rows. Assertions exist once, in `backend_common_cases.cpp`; rows come from
one builder (`addSubjectRows`), so a case cannot silently run against three of four.

### Coverage (all four rows unless stated)

identity/interfaceRevision/eight features/the six contract timings and at-rest state ·
RequestId uniqueness + completion carries its own id + **A1 submit-time stamp** + lossless
non-ASCII payload (`ünïcödé-版本`) · null/duplicate/unknown registration · removal during
delivery (self and other) · addition during a callback · **addition between production and
delivery** · non-re-entrant delivery over nine mutating paths incl. the live-view clear and a
mutation issued from a callback · **A2 Superseded marking + submit generation + empty payload +
consumer would reject** · the r2/B3 re-issue obligation asserted on the controller's own data ·
detach leaves the controller running (re-attach proves it) · managed core + external controller
alive together, `stop()` ends only the managed one, `StopCompleted` confirmed/Ok/**post-bump
(B1)**, external still answering · failed validation leaves the running config intact (both
paths active while validating, no state event, `ValidationFailed` on the candidate's id) ·
provider coalescing returns the outstanding id and settles once · TUN read-back in **both**
directions (never an echo; loopback/staged read-back, no TUN device) · **C1** `coreRunning`
true *and* false through the host-owned privileged seam.

Not claimed and left with the specialists: readiness deadlines/hard cap, probe-cancel ordering,
unconfirmed stop, `AbortOrdering`/`StopStamping` inversions — they need `core::CoreTimings` or a
knob a shipping backend must not have. Stated in both headers.

## 2. Registration (coordinator-owned; already done and matching)

`tests/core/mihomo/CMakeLists.txt` as it now stands is exactly what this package needs:
sources `contracts/backend/common/backend_common_cases.cpp` +
`support/backend/common_contract_driver.cpp` added to the **existing**
`backend-real-contract-tests`; deps `+ clash_module_host clash_component_marshal
clash_backend_fake`; `add_dependencies(... clash_qt_backend_module clash_qt_fake_backend_module)`;
`ENVIRONMENT` gains `CLASH_QT_BACKEND_MODULE` and `CLASH_QT_FAKE_MODULE`.
No new target ⇒ the sealed private impl gains no consumer and the six-target G2 allowance is not
widened. Do **not** add these sources to `clash_backend_fake`: that static library would then
need `clash_mihomo_impl` and the module host.

Optional, if a selected invocation is wanted:
`add_test(NAME backend-common-contract COMMAND backend-real-contract-tests sharedIdentityAndPublishedBudget sharedRequestIdentityAndLosslessPayloads sharedObserverRegistrationIsExplicit sharedRemovingAnObserverDuringDeliveryIsSafe sharedObserverAddedDuringACallbackSeesOnlyLaterEvents sharedObserverAddedBeforeDeliverySeesNoEarlierEvents sharedDeliveryIsNeverReentrant sharedAbandonedCompletionIsMarkedSuperseded sharedNonEndpointBumpReissuesTheSnapshotSet sharedDetachLeavesTheAttachedControllerRunning sharedStopTerminatesOnlyTheManagedCore sharedFailedValidationLeavesTheRunningConfigurationIntact sharedDuplicateProviderRequestIsCoalesced sharedTunCompletionIsAReadBackNotAnEcho sharedPrivilegedStatusCarriesTheHelpersRunningCore)`
with the same ENVIRONMENT and `clash_qt_isolate_test`. Measured wall time for the whole
executable (Debug, 94 cases): 12.5 s.

## 3. Verification

External snapshot only: `/tmp/clash-qt-p4.w0Y8Uo/common/snap` (repo `src/` + `tests/` as of the
start of this lease, plus this package), standalone `CMakeLists.txt` there, build in
`/tmp/clash-qt-p4.w0Y8Uo/common/build`. `build/` and `dev/` in the repository were never
configured, built or touched; no submodule was altered.

Final clean run (`run.sh`, log `run-uCdgHF/out.txt`):
**94 passed, 0 failed, 0 skipped**, 6 XFAIL (the two gates below), 12535 ms.
Every specialist case still passes.

Every run: isolated `CLASH_QT_DATA_DIR`, `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`, explicit `CLASH_QT_FAKE_CORE`/`CLASH_QT_BACKEND_MODULE`/
`CLASH_QT_FAKE_MODULE`. `~/Library/Preferences/com.clash-qt.clash-qt.plist` hashed before and
after **every** run: `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`
unchanged throughout (run.sh logs both into each `run-*/hashes.txt`). Nothing connected to the
installed privileged helper, the system proxy, VPN/network settings, the trust store, a real
subscription or Clash Verge discovery; no TUN device was created.

### Inversions (12 applied to the snapshot, built and run there, reverted after each)

See `inversions-batch1.md`, `inversions-batch2.md`, `inversions.py`.

| mutation | rows that failed | verdict |
|---|---|---|
| I1 stale stamp, double, ordinary completion path | — | **survived**: on that path nothing bumps between submit and delivery, so the two stamps coincide. Redundant site, not a coverage gap; I1b is the real inversion |
| I1b stale stamp, double, **abort path** | direct-fake, module-fake | detected |
| I2 stale stamp, real backend `publish()` | direct-real, module-real | detected |
| I3 `ByteWriter::text` narrowed to Latin-1 | module-real, module-fake (direct rows unaffected) | detected — proves the module rows really cross the codec |
| I4 TUN `actual` echoes the request, double | direct-fake, module-fake | detected |
| I5 TUN `actual` echoes the request, real client | direct-real, module-real | detected |
| I6 host shim skips its observer re-check | module-real, module-fake | detected |
| I7 one `scheduleSnapshotReissue()` call site removed | — | **survived**: the invalidation path still owed and discharged it. Redundant site |
| I7b re-issue disabled outright, real | direct-real, module-real | detected |
| I7c re-issue disabled outright, double | direct-fake, module-fake | detected |
| I8 helper `coreRunning` hard-coded false | direct-real, module-real | detected |
| I9 observer admission watermark removed from the double | direct-fake | detected |

10/12 detected; both survivors are explained above and each is superseded by a mutation at the
site that matters.

## 4. Open gates (reported, not asserted away)

Both are recorded as `QEXPECT_FAIL(row, "OPEN GATE …", Continue)` scoped to the rows that
exhibit them. The assertion text is unchanged and QTest reports an unexpected PASS as a
failure, so a marker cannot outlive its fix.

### G-ATTACHED-AT-REST — owner: `src/core/mihomo` (not COMPONENT-ABI)

A freshly constructed real backend reports itself **attached** to a controller nothing pointed
it at. `core::Endpoint` defaults to `127.0.0.1:9090` (`core/types.h:75-77`) and
`MihomoBackendImpl::isAttached()` is `client_.endpoint().isValid()`
(`mihomo_backend.cpp:894`, `attachmentOwnership()` at `:897`). Section 1 defines Attached as
"any endpoint the user or discovery pointed us at" and `Ownership::None` as "nothing is
attached". Both doubles answer `None` — fake and real diverge on a published query, the r3 B3
class exactly. `isExternalControllerConnected()` is gated on `isConnected()`, which is what
keeps the UI from showing it today.

Minimal reproducer: `core::MihomoBackendImpl backend;` then
`QCOMPARE(backend.attachmentOwnership(), cb::Ownership::None);` — observed `Attached (2)`;
`QVERIFY(!backend.isAttached())` fails likewise. Rows `direct-real`, `module-real` of
`sharedIdentityAndPublishedBudget`.

### G-MODULE-OBSERVER-WATERMARK — owner: COMPONENT-ABI

`ModuleBackend` keeps observers in a plain vector with no registration watermark
(`module_backend.cpp:278-289`, `forEachObserver` at `:305`), while `FakeBackend`
(`:111-116`) and `MihomoBackendImpl` (`:536-542`) stamp each observer with the event sequence
it joined at. So an observer registered **after** a mutating call produced its events but
**before** any was delivered receives them, contrary to backend-r4 §7 rule 3.

Minimal reproducer (rows `module-real`, `module-fake` of
`sharedObserverAddedBeforeDeliverySeesNoEarlierEvents`):

```
backend.addObserver(&first);
backend.attach(runningController);      // produces endpointChanged + the empty live view
backend.addObserver(&late);             // registered before ANY of it was delivered
drain();
// late.endpoints is NOT empty and late.liveStateClears != 0 on both module rows;
// both in-process rows deliver none of it.
```

**Coordination note.** COMPONENT-ABI is implementing exactly this right now:
`fake_backend.cpp`/`mihomo_backend.cpp` gained a published `deliveringSequence_` (12:12) and
`module_backend.{h,cpp}` an `admitAfter`/`QueuedEvent` design (12:15). That tree does **not
compile** at the moment it was checked (`module_backend.cpp:437`, `event.second` on the new
`QueuedEvent`), so the claim could not be verified against it and this package's evidence is
against the pre-12:12 tree plus its own files. **When that work lands, delete the two
`QEXPECT_FAIL("module-real"/"module-fake", kWatermark, …)` lines in
`backend_common_cases.cpp::anObserverAddedBeforeDeliverySeesNoEarlierEvents`** — otherwise the
rows XPASS and the suite goes red, which is the intended alarm.

## 5. Divergences that are NOT gates

* **Re-issue identity.** A backend-initiated snapshot re-issue carries `RequestId::Invalid` in
  the real backend (deliberate, commented at `mihomo_backend.cpp:562-575`) and a minted id in
  the fake. `types.h:44-50` permits Invalid for "an event that completes no request of the
  consumer's", so both are admissible; the shared case therefore asserts the **controller's own
  data** at or after the post-bump generation rather than an id. First attempt asserted the id
  and failed the real rows — that is why `countAtOrAfter` takes a payload.
* **A refused TUN change's completion status.** The real backend marks the completion failed and
  explains why; the double reports `Ok` with `actual == false`. backend-r4 fixes only that
  `actual` is a read-back, so the shared case asserts `requested`/`actual` and not the status.
  Contract owner may want to settle it.
* **A synchronously answering privileged service.** `MihomoBackendImpl::requestPrivilegedServiceStatus`
  reads `serviceStatusRequest_` *after* `process_.requestServiceStatus()`
  (`mihomo_backend.cpp:1153-1157`), so a service that answers from inside that call makes the
  facade return `RequestId::Invalid` for an operation it performed — i.e. "rejected" for work
  that completes. No real helper does that (it answers on a socket), so the driver's stub
  answers queued; recorded here because it is a live read-after-callback fragility.
* **Fixture note, not a finding.** The managed controller must listen with an empty secret: the
  backend parses its endpoint out of the configuration file, which carries no token, so a
  secret-checking fixture answers every readiness probe 401. Cost one round; encoded in
  `RealFixtures::kManagedIndex` with the reason.
