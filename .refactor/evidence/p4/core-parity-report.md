# CORE PARITY FIX — registrations, proofs, and what is still open

Worker: claude-opus-5 (explicit Opus, no sub-agents). Lease:
`src/core/mihomo/mihomo_client.{h,cpp}`, `tests/core/controller_test.cpp`,
`tests/core/mihomo/backend_real_contract_test.cpp`,
`tests/contracts/backend/common/**`,
`tests/support/backend/common_contract_driver.{h,cpp}`. No other file was
written; no git command was run; no repository build tree was configured or
built; no submodule was touched.

## 1. Defect 1 — G-ATTACHED-AT-REST, closed

`core::Endpoint` defaults to the DISCOVERY default `127.0.0.1:9090` and
`isValid()` accepts it, so `MihomoClient::endpoint_` was a valid endpoint from
construction. `MihomoBackendImpl::isAttached()` is `client_.endpoint().isValid()`
and `attachmentOwnership()` is `ownershipOf(client_.endpoint())`, so a freshly
built real backend answered `Attached` for a controller nothing had pointed it
at, while both doubles answered `None` (the r3 B3 divergence class).

Fix, inside the lease: `MihomoClient::detachedEndpoint()` (new, public static —
empty host, port 0, empty secret) is the member initialiser for `endpoint_`, and
`detach()` now assigns it instead of open-coding the same three lines.
`core::Endpoint`'s default is untouched; `core::discoverEndpoint()` is untouched
and still falls back to `127.0.0.1:9090`. Discovery hands out a guess; the guess
becomes an attachment only when `attach()`/`setEndpoint()` accepts it.

Two knock-on effects, both wanted and both asserted:

* the FIRST attach is now a real change, so `endpointChanged` is published for
  it. It used to be SILENT whenever the controller sat on the default port —
  the common case for a managed core, and for `main.cpp:352`
  `bridge.attach(backend.discoverEndpoint())` when discovery finds nothing.
  Coordinator integration note: expect one additional early `endpointChanged`
  in an application launch transcript.
* a REST call issued before any attach no longer dials `127.0.0.1:9090`, i.e.
  whatever happens to be listening there on the developer's machine.

`QEXPECT_FAIL("direct-real"/"module-real", kAttachedAtRest, Continue)` — four
lines in `backend_common_cases.cpp::identityAndPublishedBudget` — are gone. The
two assertions are now `QVERIFY2` with the contract text spelled out.

## 2. Defect 2 — identical-endpoint replacement, closed

W02's real-engine finding (`w02-fix/logs/w02-real-probe3.log`: ten
`Connection refused`, `connected false`, an explicit `refreshVersion()` healing
it with the generation unchanged): a reload rebinds the replacement engine to
the port the retired one held, `setEndpoint()` took its identical-endpoint
branch — `refreshState(); return;` — and everything in flight to the retired
process stayed CURRENT. Those replies landed after the replacement had answered
and each failure called `setConnected(false)`, clearing the live view.

Fix, at the producer boundary (`MihomoClient::setEndpoint`, same-address
branch), in this order:

1. `++requestEpoch_` — the REQUESTS moved on, not the attachment. `endpointEpoch_`
   and `endpointChanged` are left alone: publishing an endpoint change for an
   endpoint that did not change would be a lie, and `isCurrentReply()` already
   compares both epochs, so one bump is enough to retire the work.
2. `emit invalidating()` — BEFORE the abort. `MihomoBackendImpl` bumps its
   generation and marks an outstanding TUN request superseded on this signal;
   `finished()` can run synchronously inside `abort()`, so a consumer told
   afterwards would already have accepted the reply the bump was meant to
   discard. Proved by inversion I3.
3. `finishTunChange(lastTunEnabled_, "…the controller was replaced.")`
   immediately after, which is the invariant `mihomo_backend.cpp:119-128`
   depends on to classify the cancellation as a supersession without matching
   on translated text. `finishTunChange` is guarded on `tunChangePending_`, so
   the retired engine's own PATCH confirmation cannot report a second outcome.
4. abort every running reply; each settles `superseded=true` with no error.
5. `refreshState()` — the re-issue this path owes, so `attachingEndpoint_`
   still suppresses the backend's extra `scheduleSnapshotReissue()`.

Deliberately NOT done here: `setConnected(false)` and `clearLiveState()`. The
replacement is reachable at the address we are already on; clearing would make
the client produce exactly the spurious "disconnected" the defect is about, and
`refreshState()` is what confirms or denies the session. The streams are left to
their own reconnect, which re-dials the same address.

## 3. Required module rows — skips replaced by failures

`BackendRealContractTest::runShared` no longer `QSKIP`s on
`driver->unavailableReason()`: it is a `QVERIFY2` naming the subject and the
reason. A failed load already failed through `QVERIFY2(driver->prepare(), …)`.
No mock stands in for a module. The `unavailableReason()` doc comment and the
driver header's overview were corrected to say so.

## 4. Build registrations

**None are needed.** `tests/CMakeLists.txt:27-29` already builds
`controller-tests` from `core/controller_test.cpp` against
`Qt6::Test Qt6::Network clash_mihomo_impl`, and the new cases add no include,
no symbol and no fixture beyond what that file already used
(`TestController`, `QSignalSpy`). `tests/core/mihomo/CMakeLists.txt` already
carries the shared sources, both module artifacts and their environment; the new
specialist case uses `testsupport::LoopbackServer`, which that target already
links. No new target, so the six-target G2 allowance is not widened.

New API names: `core::MihomoClient::detachedEndpoint()` (public static).
New test cases: `ControllerTest::aFreshClientIsAttachedToNothing`,
`ControllerTest::reattachingToAReplacedEngineRetiresItsInFlightWork`,
`ControllerTest::aReplacedEngineCancelsAnOutstandingTunChangeExactlyOnce`,
`BackendRealContractTest::aReplacementAtTheSameAddressCannotClearTheNewSession`.
`Recorder::connections` records every connection transition in order.

The new regression is SPECIALIST, not shared, on purpose: the hazard is a real
network reply owed by a process that has gone, and the shared row count stays at
15 × 4 as the coordinator's registration expects.

## 5. Verification — external snapshot only

`/tmp/clash-qt-p4.w0Y8Uo/common/snap` (inherited from the stable shared-contract
lease; my six files copied in, byte-identical to the repository), built in
`/tmp/clash-qt-p4.w0Y8Uo/common/build`. The snapshot `CMakeLists.txt` gained a
`controller-tests` target mirroring `tests/CMakeLists.txt:27-29` — scratch only.
`build/` and `dev/` in the repository were never configured or built.

| run | result | log |
|---|---|---|
| `controller-tests` (all), final ABI | **21 passed, 0 failed, 0 skipped** | `logs/final-controller.log` |
| `backend-real-contract-tests` (all), final ABI | **95 passed, 0 failed, 0 skipped, 0 XFAIL, 0 XPASS** — 15 shared cases × 4 rows = 60, plus every original specialist | `logs/final-contract.log` |
| `controller-tests` × 3 repeats (the new cases are time-bounded) | 21/21 each | `logs/repeat-controller-{1,2,3}.log` |

34 runs are recorded in `/tmp/clash-qt-p4.w0Y8Uo/check-runs.jsonl`; the
before/after preference hash is the SAME single value in all 68 readings.

Every run through `run_check.py`: isolated `CLASH_QT_DATA_DIR`,
`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, explicit
`CLASH_QT_FAKE_CORE`/`CLASH_QT_BACKEND_MODULE`/`CLASH_QT_FAKE_MODULE`. The
native preference store was hashed BEFORE and AFTER every single run and is
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` throughout
(`check-runs.jsonl`; `run_check.py` exits 99 if it ever moves). Nothing touched
the installed privileged helper, the system proxy, VPN/network settings, the
trust store, a real subscription or Clash Verge discovery — the new tests assert
the discovery DEFAULT as a value and never dial it. Controllers are loopback
fixtures on ephemeral ports; the engine is `clash-qt-fake-core`.

### Inversions — every one applied to the snapshot, rebuilt there, observed failing, reverted

| # | mutation | observed failure |
|---|---|---|
| I1 | `endpoint_ = detachedEndpoint()` → `Endpoint endpoint_;` | `aFreshClientIsAttachedToNothing`; **and** `sharedIdentityAndPublishedBudget(direct-real)` **and** `(module-real)` — the module row proves the fix crosses the ABI |
| I2 | the whole same-address boundary removed (back to `refreshState(); return;`) | `reattachingToAReplacedEngineRetiresItsInFlightWork`, `aReplacedEngineCancelsAnOutstandingTunChangeExactlyOnce`, and `aReplacementAtTheSameAddressCannotClearTheNewSession` ("left the generation where it was") |
| I3 | abort first, `++requestEpoch_` afterwards | controller: invalidations **3** vs 2 — the aborted reply was accepted as live and announced its own invalidation by disconnecting; backend: "a reply owed by the retired engine settled as live work" |
| I4 | `emit invalidating()` dropped from that branch | controller: invalidations 1 vs 2; backend: generation not bumped |
| I5 | `finishTunChange(...)` dropped from that branch | `aReplacedEngineCancelsAnOutstandingTunChangeExactlyOnce`: completed 0 vs 1 |
| I6 | `CLASH_QT_FAKE_MODULE` pointed at an absent artifact | `sharedIdentityAndPublishedBudget(module-fake)` FAILS by name, **0 skipped** (before this lease it would have QSKIPped while CTest printed Passed) |
| I7 | `CLASH_QT_BACKEND_MODULE` pointed at a non-module file | `sharedIdentityAndPublishedBudget(module-real)` FAILS "the shipping module did not load", 0 skipped |

7/7 detected. `invert.py` applies exactly one mutation, asserts the pattern
matches once, rebuilds, runs, and restores from a byte copy; the snapshot is
byte-identical to the repository after each.

## 6. G-MODULE-OBSERVER-WATERMARK, closed against the stable ABI

`ABI_STABLE` (wire revision 2) appeared at 12:37. The stable ABI/private
dispatcher/FakeBackend sources were synchronised from the REPOSITORY into this
package's own snapshot — `src/core/component/abi/`, `src/core/mihomo/module/`
(incl. the new `shared_runtime.h`), `mihomo_backend.{h,cpp}`,
`src/integrations/component/` (host + marshal), `tests/support/backend/
fake_backend.{h,cpp}`, `tests/support/component/` — and rebuilt there. No new
.cpp file appeared, so the snapshot CMake needed no change, and the repository's
`tests/core/mihomo/CMakeLists.txt` already matches what this package needs.

Mind the mtime: `rsync -a` preserves timestamps, and ninja skipped
`com_objects.cpp` on the first attempt (link error on the new
`ByteBuffer::Create(..., ObjectAnchor *)`). The synced files are touched before
the rebuild.

Evidence that the ABI fix, not the marker, closed it: **with both markers still
in place** the two module rows XPASSed and the suite went RED exactly as the
alarm was designed to (`logs/contract-abi2-markers-still-in.log`: `XPASS …
'late.endpoints.empty()' returned TRUE unexpectedly`, twice per row). The four
`QEXPECT_FAIL(..., kWatermark, Continue)` lines were then deleted and the case
now asserts plainly. No QEXPECT_FAIL remains anywhere in this package's files;
the only remaining `QSKIP` in the suite is inside `#ifdef Q_OS_WIN`
(`backend_real_contract_test.cpp:1031`, a console-child terminate difference)
and is dead code on this platform.

I1 and I2 were re-run against the FINAL ABI tree and are still detected, so the
inversion evidence is current rather than inherited.
