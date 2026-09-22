# Independent ABI/lifetime/marshalling subgate audit — NO-GO

Read-only auditor (Opus 5). Repository never written. All work in
`/tmp/clash-qt-p4.w0Y8Uo/abi-audit/**`. No agents spawned.

## What was audited, and how that was established

Own copy of the finalized `abi-fix/snap-final` at
`abi-audit/snap`, plus the source-built engine copied to `abi-audit/mihomo`
(`6f53b2e1…5544`, `3rdparty/mihomo` @ `ab405bad`, v1.19.31, clean).

At audit start all **60** ABI/dispatcher/marshal/example/test-support files in
the copy were **byte-identical to the repository** — manifest in
`notes/repo-hashes-at-audit-start.txt`, comparison in `notes/provenance.txt`.
That includes the files the core-parity worker owns
(`mihomo_client.cpp` `f5bcb34f…567c`, `mihomo_client.h` `6a4870b9…de63`), so the
snapshot is internally consistent and not a torn read of an active file.
Re-checked after every inversion: copy pristine. Only repository drift during
the audit was `tests/contracts/component/abi/CMakeLists.txt` (coordinator-owned
build registration) — no audited source moved.

Toolchain: Darwin 25.6.0 arm64, AppleClang 17, Qt 6.11.1, yaml-cpp 0.9.
Debug **and** Release trees (`build`, `build-rel`). Never configured or built in
`build/` or `build/dev` of the repository.

Isolation: every process through `run_check.py` — fresh `CLASH_QT_DATA_DIR`,
`QT_QPA_PLATFORM=offscreen`, `QT_QUICK_BACKEND=software`, and
`com.clash-qt.clash-qt.plist` hashed before and after. It read
`681784383d6e4072c8c3b86abfb1f7a6f848a0f3af11e022223d70bb4178371c` before and
after **every** run, including the two lldb runs (hashed by hand). It never
changed. Runs appended to `check-runs.jsonl`.

No app launch, no helper connection, no system proxy/VPN/trust store, no real
subscription, no Clash Verge discovery, no submodule touched. The engine binds
`127.0.0.1:29341`/`:29342` with `mixed-port: 0` and `tun.enable: false`.

---

## NO-GO 1 — the final session Release from another thread crashes the host

This is the review's own "Additional qualification case" and it **fails**.

`core/component/abi/backend_abi.h:25` states, of the shipping vtables:
"AddRef/Release remain callable from any thread, because component-r1 says so of
every IObject." Nothing in the module honours that for the *final* release.

Path: `BackendSession::Release()` (`backend_session.cpp:95`) runs
`delete this` on the calling thread (`:98`); `~BackendSession()` calls
`Close()` (`:68`); `closeImpl` pumps
`QCoreApplication::processEvents` (`:703`) and then destroys the wrapped
`MihomoBackendImpl` — which owns the `QProcess`, its socket notifiers and a
`QNetworkAccessManager` — at `:723`, on that same foreign thread. There is no
affinity check, no `deleteLater`, no deferral to the owner thread, and no
mechanism to keep the module counted until such cleanup drained.

Reproducer: `probe/audit_probe.cpp` mode `cross-thread-release`. Activate a
session with the real engine on the main thread, then perform the last
`Release()` from a `std::thread`.

Observed, Debug **and** Release:

```
       activated; main thread=0x1016b6260
       releasing final reference from thread 0x1016c72c0
QSocketNotifier: Socket notifiers cannot be enabled or disabled from another thread
QSocketNotifier: Socket notifiers cannot be enabled or disabled from another thread
ok     the final Release from another thread returned 0
       live objects after cross-thread release: 0
QSocketNotifier: Invalid socket 7 with type Read, disabling...
* thread #1, queue = 'com.apple.main-thread', stop reason = EXC_BAD_ACCESS (code=1, address=0x7b)
    frame #0: QtCore`QSocketNotifier::setEnabled(bool) + 20
```

exit `-11` (SIGSEGV), recorded in `check-runs.jsonl` as `audit-cross-thread` and
`audit-rel-cross-thread-release`. Logs: `notes/probe-cross-thread.log`,
`notes/cross-thread-lldb.log`, `notes/rel-cross-thread-release.log`.

Note the second-order defect: `LiveObjectCount()` answered **0** while the Qt
cleanup was still outstanding, so a loader would have been told it was safe to
unmap. The review required that "the library remains counted/pinned until that
cleanup completes"; it is not.

A method-affinity note in the header would not close this, because the header
currently asserts the opposite. Either the release path must defer collaborator
destruction to the owner thread and stay counted until it drains, or
component-r1's refcount rule must be narrowed by the coordinator — which is not
a worker's call.

## NO-GO 2 — a retained module root leaks the image permanently

The refusal works; the recovery does not. `ModuleLoader::unload()` detaches its
own root handle at `module_loader.cpp:264` *before* it knows whether the image
may go, discovers a non-zero remainder, keeps the mapping and returns **true**
with `kFalse` (`:280`). `module_` is now null, so `isLoaded()`
(`module_loader.h:99`) is false and any retry hits
`module_loader.cpp:235` — `kInvalidState, "no module is loaded"`.

Meanwhile `PrepareUnload()` already latched `kUnloadingBit`
(`backend_module.cpp:138`), so the module also refuses to create any further
object. The result is a module that is simultaneously unusable and un-unloadable
for the life of the process.

Reproducer: `probe/audit_probe.cpp` mode `retained-root`.

```
ok     unload() reports the release succeeded
       lastError=1 'another reference to the module root is still held'
ok     image correctly KEPT while the root is retained
ok     releasing the retained root destroys it
       retry unload()=0 lastError=-9 'no module is loaded'
FAIL   RETRY of unload() succeeds after the retained root is released
FAIL   image is UNMAPPED after release-and-retry (not leaked forever)
```

`notes/probe-retained-root.log`. The loader's own header (`module_loader.h:67-81`)
documents the refusal but promises no way out of it; the review asked that a
retained root "must actually unload … not leak forever or latch unsafe state",
and both halves of that are missed.

## NO-GO 3 — QDateTime host-visible representation is silently changed

The review's marshalling audit required: "Preserve the host-visible
representation or document a coordinator-approved semantic choice." Neither was
done — `abi-fix/registrations.md` does not mention date-time at all.

`backend_marshal.cpp:21` stores only `toMSecsSinceEpoch()`; `:29` decodes with
`QDateTime::fromMSecsSinceEpoch(ms)`, which yields **local time**. The packed
snapshot does the same at `connection_snapshot.cpp:75,79`. `operator==` compares
instants and therefore succeeds, exactly as the review warned.

Observed (`probe/audit_probe.cpp` mode `datetime`,
`notes/probe-datetime.log`):

```
UTC                    in='2026-09-22T10:30:00Z'        out='2026-09-22T18:30:00'  spec 1->0 offset 0->28800
fixed offset +05:45    in='2026-09-22T16:15:00+05:45'   out='2026-09-22T18:30:00'  spec 2->0 offset 20700->28800
named zone Tokyo       in='2026-09-22T19:30:00+09:00'   out='2026-09-22T18:30:00'  spec 3->0 offset 32400->28800
local                  in='2026-09-22T18:30:00'         out='2026-09-22T18:30:00'  spec 0->0 (unchanged)
packed start           in='2026-09-22T10:30:00Z'        out='2026-09-22T18:30:00'
```

`operator==` passed on every row; `toString(Qt::ISODate)` failed on five.
`src/ui/pages/connections/connections_page.cpp:57-58` renders
`connection.start/end.toString(Qt::ISODate)`, so this is user-visible text that
changes when the backend moves behind the module. An invalid `QDateTime`
correctly survives as invalid — that part is fine.

D8 intends no UI change, so this needs a codec that carries the spec/offset (or
an explicit coordinator decision), not a passing equality test.

---

## Minor / informational

* **Linux and Windows pinning paths are not "compiled".**
  `registrations.md` §1 says of `shared_runtime.h` "Both are compiled, neither is
  run", and §5 repeats it. On Darwin the `#elif defined(__linux__)` and
  `#elif defined(_WIN32)` branches are preprocessed away — a preprocessor run of
  the header emits **zero** tokens from them — and no cross toolchain exists on
  this machine (`x86_64-linux-gnu-g++`, `aarch64-linux-gnu-g++`, `clang-cl`,
  `x86_64-w64-mingw32-g++`, `zig` all absent; no sysroot). No Windows/Linux
  *qualification* is claimed anywhere, which is correct; the *compiles* claim is
  simply unsupported and should be struck. This audit makes no
  Windows/Linux claim of any kind.
* **A decode failure detected inside `dispatch()` leaves no diagnostic.**
  `backend_session.cpp:259-261` returns the failure without `setError`, so
  `LastError()` answers `kNotFound` (observed: `-5`, null out) after a refused
  `kCmdSetMode`. The ABI says a diagnostic "may" accompany a failure, so this is
  not a contract violation — but the suite case is named
  `aMalformedArgumentBlockIsRefusedWithADiagnostic` and asserts only the
  `Result` (`abi_contract_test.cpp:1556-1573`), so the name overstates it.
* **Known surviving mutant, confirmed as redundancy not gap.** Replacing
  `module_backend.cpp:418`'s whole guard with `if (false)` survives the suite.
  The invariant is independently enforced at `:446`
  (`if (delivering_ || commandDepth_ > 0) return;`), so the enqueue-side guard is
  genuine defence in depth. Consistent with the code's own note at `:423-432`,
  and broader than the mutant that note records.

---

## What was verified and holds

Each of these was proved by an **observed inversion failure on the external
copy** (`probe/inversions.py`, results in `notes/inversions.json`); 10 of 11
inversions detected, the 11th explained above.

| Claim | Evidence | Inversion that detects it |
|---|---|---|
| Activated shipping module, real engine, ready + **confirmed** stop + drain, image unmapped, host exits 0 | `notes/probe-real-engine.log`, `notes/rel-real-engine.log` (Debug and Release) | `pinning-is-a-noop` → SIGSEGV (exit 245) |
| Qt runtime stays mapped, component does not | QtCore + QtNetwork present, `clash_qt_backend_module` absent from the dyld list and from `RTLD_NOLOAD` | same |
| Reply buffer + diagnostic + **nested** message buffer each counted after session release; unload refused until released, then unmaps | `notes/probe-buffers.log`, live count exactly 3 | `reply-buffer-not-anchored`, `error-info-not-anchored` |
| `CreateObject` vs `PrepareUnload` is one atomic transition | 3000 independent races, 2914 creations, 2977 quiescent answers, **23 genuinely lost races**, 0 objects created after the latch (`notes/probe-concurrency.log`) | `create-object-check-then-increment` |
| Handshake validates response size before **every** write, incl. null request/output | canary intact past a genuinely short struct; null response allowed; stale wire revision refused (`notes/probe-handshake.log`) | `handshake-size-checked-late` |
| Factory / allocation / error containment at COM entry points | null out, unknown class, unsupported interface, closed session, test-control range — all refuse with a nulled output and **zero** leaked count (`notes/probe-containment.log`) | — (all 19 assertions pass; no inversion attempted) |
| Observer admission by **production**, real engine | late observer received 0 of 8 log lines / 2 state changes; mid-delivery registrant received 7 where the registrar received 8 (`notes/probe-observers.log`) | `observer-admitted-on-arrival-not-production`, `event-envelope-always-zero` |
| Removal during delivery is honoured | contract suite | `removal-during-delivery-ignored` |
| Lossless 64-bit counters/generation/totals, 4-byte UTF-8, empty and null strings, binary64 rates, chains | `notes/probe-marshalling.log` | — |
| Packed-buffer bounds: magic, version/recordSize, count, chainSlots, recordOffset, chainOffset, blobOffset, blobSize each refused by name; 96 single-byte header corruptions refused with a reason; **every** truncation refused; null buffer refused | same | `snapshot-bounds-unchecked` |
| No helper client, no platform imports, no proxy-traffic marshalling in the module | `otool -L` shows only Qt/yaml-cpp/system frameworks; zero `PrivilegedServiceClient`/`SystemProxy` symbols; no `platform/`, `QLocalSocket` or `privileged_service_client` include under `core/mihomo/module/` or `core/component/`; `wire.h` carries lifecycle/control/telemetry codes only | — |
| Independently loaded third-party consumer | `examples/component_consumer` configured and built as a **standalone** CMake project against the published headers + QtCore + `dl` only, then drove the pinned local engine to ready and confirmed stop, image unmapped, exit 0 (`notes/sample-run.log`) | — |
| Suites reproduce | `abi-contract-tests` 35/35 Debug **and** Release; `connection-snapshot-marshal-tests` 15/15 Debug **and** Release — the Debug ratio failure the implementer reported is no longer present | — |

## Per-target evidence

| Target | Build | Result | Log |
|---|---|---|---|
| `abi-contract-tests` | Debug | 35 passed | `notes/probe-*`/`check-runs.jsonl` `audit-baseline-dbg` |
| `abi-contract-tests` | Release | 35 passed | `notes/rel-contract.log` |
| `connection-snapshot-marshal-tests` | Debug | 15 passed | `notes/dbg-bench.log` |
| `connection-snapshot-marshal-tests` | Release | 15 passed | `notes/rel-bench.log` |
| `audit-probe` real-engine | Debug / Release | 0 failures | `notes/probe-real-engine.log`, `notes/rel-real-engine.log` |
| `audit-probe` cross-thread-release | Debug / Release | **SIGSEGV** | `notes/probe-cross-thread.log`, `notes/rel-cross-thread-release.log` |
| `audit-probe` retained-root | Debug | **2 failures** | `notes/probe-retained-root.log` |
| `audit-probe` datetime | Debug | **5 failures** | `notes/probe-datetime.log` |
| `audit-probe` buffers / concurrency / handshake / marshalling / containment / observers | Debug | 0 failures | `notes/probe-*.log` |
| `component_consumer` (standalone project) | Release | exit 0, unmapped | `notes/sample-run.log` |

## Verdict

**NO-GO for the ABI/lifetime/marshalling subgate.** The central claim the review
reopened — an activated shipping module genuinely unmapping after a real engine
session, with the shared Qt runtime retained rather than the component — is
independently reproduced and holds, through a different host and a genuinely
independent consumer. Three of the review's own follow-up requirements do not:
cross-thread final release (crash), retained-root release-and-retry (permanent
leak plus a latched-unusable module), and the date-time representation
(changed and undocumented). None was fixed or documented here, as instructed.

This qualifies the ABI/lifetime/marshalling subgate only; the full fresh-clone
P4 gate is still pending. Claims are limited to measured macOS arm64 /
AppleClang 17 / Qt 6.11.1.
