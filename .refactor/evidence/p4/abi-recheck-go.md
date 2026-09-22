# Independent FINAL ABI recheck — evidence and registrations

Read-only recheck worker (Opus 5). No repository write of any kind, no git, no
agents. Everything under `/tmp/clash-qt-p4.w0Y8Uo/abi-recheck/**`; every other
scratch package and the repository were read only.

## Provenance of what was tested

Own copy of the repair worker's finished stable snapshot:

* `abi-final-fix/snap` → `abi-recheck/snapshot-final`
* `abi-final-fix/snap-pristine` → `abi-recheck/snapshot-pristine`
* `diff -rq snapshot-final snapshot-pristine` → **identical**, so the copy the
  repair worker left is the copy it proved, not a later edit of it.
* `notes/provenance.txt`: at recheck start **78 of 81** files in that copy were
  byte-identical to the repository. The three that differed were
  `controller_discovery.{h,cpp}` and `mihomo_client.cpp` — the isolation /
  transport workers' files, none of them on the ABI surface.
* Engine `abi-recheck/mihomo` sha256
  `6f53b2e18687b26c9948a804ab9e7b1b4421e8d0d3a391c11e8468dd1685b544`, the same
  source-built `3rdparty/mihomo` @ `ab405bad` (v1.19.31) the auditor used.
  Submodule status clean and untouched.

Active-worktree files were deliberately NOT compiled from the repository: by
mid-recheck `backend_session.{h,cpp}`, `backend_abi.h` and
`abi_contract_test.cpp` had drifted (the RC worker), and
`controller_discovery`/`mihomo_client` had drifted (transport/isolation). Only
the frozen copy was built.

Toolchain: Darwin 25.6.0 arm64, AppleClang 17.0.0.17000603, Qt 6.11.1, yaml-cpp
0.9 (`-Dyaml-cpp_DIR=/opt/homebrew/lib/cmake/yaml-cpp`). Debug (`build`) and
Release (`build-rel`) trees, plus `build-inv` for mutants and `build-sample` for
the standalone consumer. Never configured or built in the repository's `build/`
or `build/dev`.

Isolation: every process through `../run_check.py` — fresh `CLASH_QT_DATA_DIR`,
`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, and
`~/Library/Preferences/com.clash-qt.clash-qt.plist` hashed before AND after.
Every run read `681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c`
on both sides; it is still that value. Runs are in `../check-runs.jsonl` under
`rc-*` and `rcinv-*`.

No helper was contacted or touched, no system proxy, no VPN, no network
settings, no trust store, no real subscription, no Clash Verge discovery. The
engine fixtures bind `127.0.0.1:29361/29362/29363` (deliberately different ports
from every other package), `mixed-port: 0`, `tun.enable: false`. Service mode is
driven through a fake injected host seam, never a helper. After every
engine-driving run no `abi-recheck/mihomo` process remained.

---

## 1. The whole matrix, re-run independently

`probe/matrix.py`, results in `notes/rc-matrix.log`. Both configurations. The
repair worker's `fix_probe.cpp` and the auditor's `audit_probe.cpp` were copied
byte-for-byte and run unmodified.

| Target | Debug | Release |
|---|---|---|
| `abi-contract-tests` | 40 passed, 0 failed | 40 passed, 0 failed |
| `connection-snapshot-marshal-tests` | 16 passed, 0 failed | 16 passed, 0 failed |
| `audit-probe` real-engine / datetime / buffers / handshake / marshalling / containment / observers / cross-thread-release | 0 failures each | 0 failures each |
| `audit-probe` retained-root | 3 failures (by design, below) | 3 failures |
| `audit-probe` concurrency | 1 failure (by design, below) | 1 failure |
| `fix-probe` all five modes | 0 failures each | 0 failures each |

The three NO-GO defects are gone at their own reproducers: `datetime` 5 → 0,
`cross-thread-release` SIGSEGV → 0 failures, and the two retained-root NO-GO
assertions ("RETRY of unload() succeeds", "image is UNMAPPED after
release-and-retry") now PASS inside the auditor's unmodified probe.

### The two old-probe failures are caller-protocol changes, not weakened tests

I read both rather than accepting the repair worker's account.

`retained-root` (3 failures) — all three encode the OLD design:
`unload()` returning true on a refusal, the refusal being `kFalse`, and
`retained->Release()` reaching 0. Under the coordinator's protocol a refusal
returns **false** with `kInvalidState`, and the loader deliberately still pins
the root, which is what makes the retry possible. The probe's two NO-GO
assertions pass. No assertion was weakened; the design changed underneath them.

`concurrency` (1 failure) — the probe races a caller that still holds its root
reference, so `PrepareUnload` now refuses for a reason unrelated to the race and
"answered quiescent sometimes" is vacuous (`preparedQuiescent=0
refusedAlive=3000`). The same experiment under the protocol is `fix-probe
--mode concurrency-protocol`: **3000 rounds, 2937 creations, 2909 quiescent
answers, 91 genuinely lost races, 0 objects created after the latch.** That is
not a weaker test; it is the same test with a caller that obeys the new rule.

---

## 2. My own probe: `probe/recheck_probe.cpp`

Written against the published headers, not derived from either existing probe.
Three modes, `recheck-probe --mode …`. 0 failures in Debug AND Release for the
first two; mode 3 is the RC clause and is reported separately.

### `wire-timestamps` — representation across the REAL boundary

The repair worker proved the codec in-process. This drives the whole ABI
boundary: host writes a connection set with `kFakeEmitConnections`, the module
decodes it and re-emits it through the **production packed snapshot codec**, the
host unpacks it as a `kEvtConnectionsUpdated` event. Eight rows, each `start`
and `end` carrying a DIFFERENT representation, compared on a four-part shape —
`toString(Qt::ISODate)`, `timeSpec()`, `offsetFromUtc()` and the zone id — not
on `operator==` and not on ISODate alone.

Observed, each preserved exactly:

```
UTC             2026-09-22T10:30:00Z      spec=1 off=0      zone=UTC
local           2026-09-22T18:30:00       spec=0 off=28800  zone=Asia/Shanghai
offset +05:45   2026-09-22T16:15:00+05:45 spec=2 off=20700  zone=UTC+05:45
offset -09:30   2026-09-22T01:00:00-09:30 spec=2 off=-34200 zone=UTC-09:30
zone Asia/Tokyo 2026-09-22T19:30:00+09:00 spec=3 off=32400  zone=Asia/Tokyo
offset +09:00   2026-09-22T19:30:00+09:00 spec=2 off=32400  zone=UTC+09:00
America/St_Johns 2026-09-22T08:00:00-02:30 spec=3 off=-9000 zone=America/St_Johns
invalid         <invalid>
```

Rows 5 and 6 are the point: `Asia/Tokyo` and a fixed `+09:00` render to the
IDENTICAL ISODate string, so an ISODate-only assertion cannot tell a codec that
swapped one for the other. The probe asserts both that they render identically
and that the boundary still distinguishes them (`spec=3` vs `spec=2`).

Refusals, each by name rather than by a bare false:
`"a timestamp names an unknown time representation"`,
`"a timestamp offset is outside the representable range"`,
`"a timestamp names a zone this build cannot resolve"`; and at the general
codec, an unknown spec, an out-of-range offset, an empty named zone and
`Mars/Olympus_Mons` each latch the reader (so the containing command is refused
as `kInvalidArgument`), while `kNoTimestamp` decodes as an invalid QDateTime and
is explicitly NOT a decode failure.

### `root-holders` — the recoverable unload for every kind of holder

The repair worker's case retains `kIObjectId`. This repeats the whole protocol
for **each** interface a foreign consumer can hold — `kIComponentModuleId`,
`kIModuleLifetimeId`, `kIObjectId` — and interleaves a live session object:

per holder, all green in Debug and Release —
unload REFUSES (`kInvalidState`, "another reference to the module root is still
held; release it and retry"); image kept; root restored; `createSession()` still
works (no latch) and is counted; a second refusal while that object is alive is
reported differently and correctly ("1 object(s) from this module are still
alive"); the object is released; a THIRD refusal behaves like the first; the
foreign interface is released leaving 2 references (the loader's); the RETRY
succeeds; the image is unmapped — confirmed by the dyld image list AND by an
independent `dlopen(RTLD_NOLOAD)` in the probe itself, so the case does not
depend on the subject's own `isImageStillMappedInProcess()`.

### `deferred-retention` — the two lifetime mechanisms compose

Neither existing probe exercises buffer/diagnostic anchoring together with a
deferred session teardown, and the interesting failure is one swallowing the
other. Measured (Debug and Release, 0 failures): a reply buffer, an `IErrorInfo`
and its NESTED message buffer plus the session are counted as **4**; a foreign
final Release leaves **4**; unload refused; after the owner thread pumps, **3**
— the session is gone and all three anchors survive; unload still refused; the
diagnostic is still USABLE (`GetSource` succeeds) after the session that made it
was destroyed; releasing the three reaches 0, unload succeeds, image unmapped.

### `release-semantics` — see §4.

---

## 3. Inversions, EXTERNAL COPY only

`probe/inversions.py`. `snapshot-final` is never mutated; a third copy
`snapshot-inv` is restored from `snapshot-pristine` before and after every case,
with mtimes bumped (the repair worker's recorded ninja/`copytree` hazard).
Baselines green first. `snapshot restored byte-identical: True`.

**13 of 15 detected**, results in `notes/inversions.json` (the last row was run
separately, by hand, because its detector is a different probe):

| Inversion | Detector | Outcome |
|---|---|---|
| `timeRepresentationOf` always answers `kTimeLocal` | recheck wire-timestamps | DETECTED (exit 1) |
| an unresolvable zone degrades to local instead of refusing | recheck wire-timestamps | DETECTED |
| `packTime` drops the zone identifier | recheck wire-timestamps | DETECTED |
| `packTime` drops the UTC offset | recheck wire-timestamps | DETECTED |
| representation unchecked on BOTH sides | recheck wire-timestamps | DETECTED |
| packed validator alone removed | recheck wire-timestamps | **SURVIVED** |
| `PrepareUnload` latches its refusal | recheck root-holders | DETECTED |
| `PrepareUnload` ignores foreign holders | recheck root-holders | DETECTED |
| loader does not restore the root on refusal | recheck root-holders | DETECTED |
| loader drops every reference before asking | recheck root-holders | DETECTED |
| the diagnostic is not anchored on the module | recheck deferred-retention | DETECTED |
| `retire()` always destroys on the calling thread (pre-RC) | recheck deferred-retention | **SURVIVED** |
| …the same mutant, against an ACTIVATED session (pre-RC) | fix-probe cross-thread-final-release | DETECTED, **exit 245 (SIGSEGV)** |
| **post-RC**: a deferred Release returns 0 anyway | recheck release-semantics | DETECTED |
| **post-RC**: the module count is dropped before destruction | recheck release-semantics | DETECTED, **exit 245** |
| **post-RC**: `mustDeferFinalCleanup` never defers | recheck release-semantics | DETECTED, **exit 245** |
| **post-RC**: the owner thread ignores pending native work | recheck release-clauses | DETECTED (Release blocked **600 ms** vs 0 ms) |

**15 of 16 detected overall.** Two survivors, neither excused:

* **`packed-validator-alone-removed`** — independently re-derived rather than
  taken from the repair worker's note: removing only the packed validator's
  range check still fails the decode, because `dateTimeFromParts` refuses on its
  own and `connection_snapshot.cpp:329` turns that into "a timestamp could not
  be rebuilt from its representation". Removing BOTH is detected. Defence in
  depth, not an uncovered path. My probe asserts a non-empty reason rather than
  a particular sentence, so it cannot tell which layer named it.
* **`retire-always-destroys` under `deferred-retention`** — my retention case
  uses an IDLE session, which owns no QProcess or socket notifier, so destroying
  it on a foreign thread is harmless there and the count still drops. That mode
  is therefore NOT credited with covering thread affinity. The activated
  detectors do cover it, twice: pre-RC through `fix-probe`, and post-RC through
  `rc-never-defers-at-all`, both **exit 245** with `QSocketNotifier: Socket
  notifiers cannot be enabled or disabled from another thread` — the auditor's
  original NO-GO 1, reproduced by me, on my own copy, by removing this fix and
  nothing else.

The owner-thread-native-work mutant initially SURVIVED and I did not leave it
there: destroying synchronously also reaches a truthful zero, because
`~CoreProcess` `waitForDone()`s the pool — it just stalls the owner's event
loop. The count cannot tell those apart, so I added a clock. Baseline Release
takes **0 ms** across three runs; the mutant takes **600 ms**. The assertion is
"Release did not BLOCK the owner's loop draining the parse", with a 250 ms
threshold well clear of both.

---

## 4. The RC clause: what a returned zero MEANS

`object.h:66` — "Only a returned 0 is reliable, and it means the object was
destroyed". The only honest external witnesses for "destroyed" are the module's
own live-object accounting (decremented by the object's destructor) and the
loader's ability to unmap; `recheck-probe --mode release-semantics` asserts
both, on an ACTIVATED session driving the real engine, for the owner thread and
a foreign thread.

Measured on the frozen pre-RC snapshot (`notes/rc-dbg-recheck-release-prerc.log`):

```
OWNER   thread: Release()=0 liveCount=0 unloadNow=1        -> all clauses hold
FOREIGN thread: Release()=0 liveCount=1 unloadNow=0        -> FAILS
                (count reaches 0 after the owner thread pumps)
```

So the crash is genuinely fixed and the module genuinely stays counted, but a
returned **0** did not mean destroyed on the foreign path. That is exactly the
tightening the coordinator assigned to the RC worker, reproduced here
independently before that worker's marker appeared.

### 4b. After `RC_STABLE` — the clause now holds

Four files were copied from the repository into my snapshot, nothing else
(`notes/rc-sync.txt`):

| File | pre-RC sha256 | post-RC sha256 |
|---|---|---|
| `src/core/mihomo/module/backend_session.h` | `8e58f603…d77e7` | `22624ebd…2f5cc` |
| `src/core/mihomo/module/backend_session.cpp` | `5244a329…17f1e` | `fa7b309f…2b9aac` |
| `src/core/component/abi/backend_abi.h` | `687ff08d…5c7b02a` | `5d935804…a97d3d4a84` |
| `tests/contracts/component/abi/abi_contract_test.cpp` | `efe8840c…77882fe` | `b5819f15…5e29223cb` |

The transport worker had by then also changed `src/core/backend/**`,
`mihomo_backend.*`, `mihomo_client.*`, `provider_client.*`,
`controller_discovery.*` and `fake_backend.cpp`. Those were deliberately NOT
copied and NOT compiled; 61 of 81 files still match the repository and the 20
that differ are exactly the active transport/isolation set plus the
coordinator-owned ABI test `CMakeLists.txt` and a README.

Re-measured, Debug and Release, 0 failures:

```
OWNER   thread, idle session:            Release()=0  liveCount=0  unload now
FOREIGN thread, activated session:       Release()=1  liveCount=1  unload REFUSED
                                          -> pump -> liveCount=0 -> unload, UNMAPPED
OWNER   thread, native parse in flight:  Release()=1  liveCount=1  unload REFUSED
                                          -> pump -> liveCount=0 -> unload, UNMAPPED
                                          and Release itself took 0 ms
extra reference held:                    Release()=1, object still usable
after Close(…):                          liveCount still 1; the later Release()=0 retires it
```

So a returned zero means the destructor ran; a non-zero return is a real
remaining count held by the cleanup; the module count stays positive until
actual destruction; the image unmaps only afterwards; and `Close`/drain is
still what yields an operation outcome — `Release` neither replaces it nor
blocks the owner's loop doing it.

`abi-contract-tests` grew 40 → **45** cases (the RC regressions), 45 passed /
0 failed in both configurations.

Two old-probe assertions now fail BY DESIGN, and only those two:
`audit-probe --mode cross-thread-release` and `fix-probe --mode
cross-thread-final-release` each assert "the final Release from another thread
returned 0". Everything else in the fix-probe case — still counted, unload
refused, image kept, loader usable, teardown ran, count zero, unload succeeds,
image UNMAPPED, component absent, QtCore retained — still passes. No lifetime or
unmap assertion was weakened; the published semantics changed underneath a
zero-return assertion, exactly as `RC_STABLE` says.

---

## 5. Everything else that was re-checked and holds

Each re-run on my copy, not taken on report:

* **Activated shipping module unmaps.** Real engine to READY and a CONFIRMED
  stop, zero live objects, `unload()` true, component absent from the dyld image
  list, QtCore AND QtNetwork still mapped — the runtime is retained, the
  component is not.
* **An independent third-party consumer.** `examples/component_consumer`
  configured and built as a STANDALONE CMake project (public headers + QtCore +
  `dl` only, no project library), driving the local engine to ready and a
  confirmed stop against BOTH the Debug and the Release module: `image
  unmapped: yes`, `teardown complete`, exit 0.
* **Buffer/error/nested-buffer retention.** After the session is released the
  module counts exactly **3** — reply buffer, `IErrorInfo`, and the nested
  message buffer — unload is REFUSED, the image is kept, and it unmaps only once
  all three are released.
* **Handshake size validation with canaries.** A short response struct, a null
  request with a short struct, a null output pointer and a stale wire revision
  are each refused with nothing written past the struct and a null root.
* **Fresh dispatch diagnostics.** Five commands, a fresh session each: the
  pre-state is "no diagnostic", the refusal carries THIS call's code and names
  the command (`0x300`, `0x303`, `0x30a`, and `0xf0000001` "is in the
  test-control range, which a shipping module does not implement"), and a later
  success clears it.
* **Atomic create-vs-unload under the protocol.** 3000 rounds, 0 objects handed
  out after the latch.
* **Native asynchronous work.** Real engine in service mode against a fake
  injected host seam (no helper anywhere in the process): native work observed
  at 1 while the parse is queued/running, `OutstandingWork()` = 2,
  `Close(timeout 0)` answers `kTimeout` and the diagnostic COUNTS the survivor
  ("1 request(s) and 1 native task(s) were still outstanding at close"), the
  runnable has exited by the time Close returns, and the image unmaps. Close and
  drain remain the way to get an operation outcome; Release does not replace
  them.

## 6. Minor / unmeasured, not inflated

* `tests/support/component/fake_module_protocol.h:72` still declares
  `kMinConnectionBytes = 4 * 13 + 8 * 4 + 8 * 2` = 100, the pre-wire-3 value;
  `marshal::kMinConnectionBytes` is now 118. It is **dead**: the only consumer,
  `fake_module_entry.cpp:205`, already uses the shipping constant. Zero
  behavioural consequence — a stale comment in constant form, in test support.
* Windows and Linux are **not compiled and not qualified** here. No cross
  toolchain exists on this machine. I make no claim about them of any kind.
* `module-count-released-in-the-destructor-body` remains **unverified by
  inversion**, as the repair worker reported. The ordering is implemented
  (`moduleCount_` is the first-declared member) and I confirmed that by reading,
  but with this class's current members the observable window is empty. I did
  not manufacture a poller to claim a proof.
* The `packed-validator-alone-removed` survivor above.
* Qualification is macOS arm64 / AppleClang 17 / Qt 6.11.1 only. Beyond the one
  Release-latency measurement above, timing, throughput and memory were not
  measured; this is a correctness recheck.
* **Harness note.** The `rc-module-count-released-before-destruction` mutant
  crashes the probe (exit 245) and that ORPHANED its fixture engine — my own
  source-built `abi-recheck/mihomo` on my own loopback port 29361, reparented to
  init. I found it with `pgrep`, confirmed the command line and the listening
  socket were mine, and terminated it; the ports are free and no engine of mine
  remains. Nothing outside this package was touched. Worth recording because any
  crashing mutant in this area can leave one behind.
* No claim is made here about the transport worker's in-flight changes to
  `src/core/backend/**`, the private backend/client/provider or `FakeBackend`.
  Those files were never compiled in this recheck, so this GO covers the ABI,
  lifetime and marshalling surface as frozen above, not the transport gate.
* `LiveObjectCount()` is a snapshot, and my release assertions read it
  immediately after Release on the same thread. That is sound for the
  deferred-cleanup case (the cleanup runs on the owner thread, which is the one
  asking) but it is not a concurrency proof, and I do not offer it as one.
