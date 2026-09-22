# ABI HARDENING — diagnosis, fixes, registrations and proofs

Worker: ABI-FIX (Opus 5). Follow-up lease on P4_ABI_REVIEW.md.
Everything below was built and run in an external snapshot at
`/tmp/clash-qt-p4.w0Y8Uo/abi-fix/snap` with its own standalone `CMakeLists.txt`.
**No repository build tree was configured, built or touched.**

Isolation on every run: fresh `CLASH_QT_DATA_DIR`, `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`. `com.clash-qt.clash-qt.plist` hashed before and
after every process, `681784383d6e...78371c` throughout, never changed. Runs
recorded in `/tmp/clash-qt-p4.w0Y8Uo/check-runs.jsonl` via the shared
`run_check.py`.

---

## 1. The dlclose crash: diagnosed, not inferred

### The stack

Staged reproducer `repro/unmap_probe.cpp`, shipping module, real source-built
engine on loopback `127.0.0.1:29141`, `mixed-port: 0`, under lldb:

```
* thread #1, stop reason = EXC_BAD_ACCESS (code=1, address=0x102071c44)
  * frame #0: 0x0000000102071c44
    frame #1: QtCore`void doActivate<false>(QObject*, int, void**) + 1344
    frame #2: QtCore`QObject::~QObject() + 348
    frame #3: unmap_probe`main at unmap_probe.cpp:268
    frame #4: dyld`start + 6992
```

It crashes AFTER `dlclose returned 0` and after `RTLD_NOLOAD` reported the
module unmapped — i.e. at `~QCoreApplication`, emitting `destroyed()`.

### Attributing the faulting address

The probe printed every image base before the unmap. Faulting PC `0x101fd1c44`,
QtNetwork base `0x101fb8000` → offset `0x19c44`.

```
atos -o .../QtNetwork -l 0x101fb8000 0x101fd1c44
  QtPrivate::QCallableObject<void (*)(), QtPrivate::List<>, void>::impl  (in QtNetwork)
```

**The retained callback is in QtNetwork, not in the module.** After the
`dlclose`, only QtCore was still mapped; QtNetwork, QtWebSockets, QtConcurrent
and yaml-cpp had gone with the module, because the probe links QtCore alone and
the module was their only holder. QtNetwork's `QHostInfoLookupManager` registers
on `QCoreApplication::destroyed` the first time a name is resolved.

Two independent confirmations:

* `qApp->disconnect()` before the `dlclose` removed the crash, and Qt printed
  `QObject::disconnect: wildcard call disconnects from destroyed signal of
  QCoreApplication::unnamed` — so qApp really was the sender.
* Staging: stages 0–3 (load / create session / SetHost / setBinaryPath) all
  unmapped cleanly and exited 0. Only stage 4 — the first to make a network
  request — crashed. **The module's own image was never the problem.**

A `QObject::connectImpl` trace over the whole run shows every module-originated
connection; none has qApp as sender. The "module registers Qt meta-objects"
explanation in the previous report is **wrong**, and its `PinnedOnceActivated`
workaround was treating a symptom in the wrong image.

### The fix, and the proof that it is the fix

New header-only `src/core/mihomo/module/shared_runtime.h`. Before anything is
constructed, the module walks its OWN Mach-O `LC_LOAD_DYLIB` list and pins each
dependency with `dlopen(RTLD_NOLOAD | RTLD_NODELETE | RTLD_LAZY)`, never
releasing the handle. `LC_ID_DYLIB` is deliberately excluded: the module's own
image is the one thing that must stay unpinned. Linux walks `_DYNAMIC`
DT_NEEDED; Windows pins imports with `GET_MODULE_HANDLE_EX_FLAG_PIN`. Both are
compiled, neither is run.

`UnmapPolicy` is **gone**. `BackendModule` takes the measured pinning result;
`PrepareUnload()` answers `kOk` when it succeeded and `kFalse` when it did not.

Measured, same probe, same engine, after the fix:

```
ready=1 failed=0 / stopped=1 / live objects: 0, prepare=0 (kOk)
dlclose returned 0
still mapped after dlclose: NO (unmapped)
after dlclose still mapped: QtCore, QtWebSockets, QtNetwork
exit 0
```

And the sample consumer, real engine, start → ready → stop **confirmed**:

```
live objects before unload: 0
unmappable: yes
image unmapped: yes
teardown complete            exit 0
```

**Gate closed: the activated shipping module is genuinely unmapped, verified
with RTLD_NOLOAD, and the process exits 0.**

---

## 2. The other review items

| # | Item | What changed |
|---|---|---|
| 1 | module-owned buffers/errors, nested buffers | `marshal::ObjectAnchor` in `com_objects.h`; `ByteBuffer`/`ErrorInfoObject` take one at `Create` and release it in their destructors. `ErrorInfoObject` stores the anchor and passes it to the buffers `GetMessage`/`GetSource` produce. `BackendModule` implements `ObjectAnchor`; the session anchors its reply buffers and diagnostics, and `BackendSession::anchor()` lets a command extension (the test double) anchor its own. `LiveObjectCount()` therefore counts every produced object, not just sessions. |
| 2a | CreateObject vs PrepareUnload race | The count and the "no more objects" latch live in ONE `std::atomic<uint64_t>` (bit 63 = latch). `CreateObject` admits-and-increments under a CAS loop; `PrepareUnload` checks-and-latches under a CAS loop. No check-to-act window. |
| 2b | retained root references | `LiveObjectCount()` excludes the root by design, so the count cannot protect it. `ModuleLoader::unload()` now performs the final `Release()` by hand and reads the answer: non-zero means somebody else holds the root, and the image is kept with a diagnostic. |
| 2c | exceptions at COM entry | `BackendModule::CreateObject` (construction and extension copy handled separately, because `~BackendSession` is private), `runModuleEntry`, `BackendSession::SetHost/Invoke/LastError/Close` (each a try/catch around an `Impl`), `ModuleBackend::Host::Notify/Invoke`, `ModuleBackend::addObserver`. All return a failure code with a null output; none can unwind into the peer's runtime. |
| 3 | handshake response size | `responseIsWritable()` is evaluated FIRST, before any write, on the null-request and null-output paths too. |
| 4 | observer admission by production | wire.h revision 2: every `Notify` payload carries a u64 production-sequence envelope, and `kCmdProducedSequence` (0x0601) answers the module's current produced sequence. `MihomoBackendImpl` and `FakeBackend` publish `producedSequence()`/`deliverySequence()`; `BackendFactory` now returns a `WrappedBackend` carrying both accessors, so **no published facade or Qt bridge signature changed** and no new IID was needed. The host records `producedSequence()` at registration and admits an event only when its envelope sequence is greater. |
| 5 | target ABI portability | `std::endian` replaces `__BYTE_ORDER__`; `kTargetCxxAbi` is "msvc" whenever `_MSC_VER` is defined, so clang-cl is classified by its ABI rather than its identity. No Windows or Linux qualification is claimed anywhere — the header says so explicitly. |

---

## 3. Build registrations

**None are required.** This is deliberate:

* `shared_runtime.h` is header-only, so it adds no translation unit to the four
  adapter sources the build already names.
* `abi-contract-tests` already links `clash_backend_fake` in
  `tests/contracts/component/abi/CMakeLists.txt`, which the two new
  direct-vs-module comparison cases need.
* The coordinator has already added `${CMAKE_DL_LIBS}` to
  `clash_qt_fake_backend_module`; `clash_qt_backend_module` needs the same on
  Linux (on macOS `dlopen` is in libSystem). That is the one thing to check.

### API names added or changed

| Name | Header | Note |
|---|---|---|
| `clashqt::integration::marshal::ObjectAnchor` | `marshal/com_objects.h` | `objectCreated`/`objectDestroyed` |
| `ByteBuffer::Create(..., ObjectAnchor*)`, `ErrorInfoObject::Create(..., ObjectAnchor*)` | same | defaulted to `nullptr` for host-side objects |
| `core::module::pinSharedRuntime()`, `pinnedImageCount()` | `module/shared_runtime.h` | new, header-only |
| `core::module::WrappedBackend` | `module/backend_session.h` | replaces the bare `unique_ptr` a `BackendFactory` returned |
| `BackendSession::anchor()` | same | for a command extension's replies |
| `runModuleEntry(..., bool imageMayUnmap)` | `module/module_export.h` | test-double-only overload; the shipping entry cannot reach it |
| `ModuleLoader::isImageStillMappedInProcess()` | `component/module_loader.h` | asks the platform loader, not our bookkeeping |
| `MihomoBackendImpl::producedSequence/deliverySequence`, `FakeBackend::producedSequence/deliverySequence` | private dispatchers | additive |
| `abi::kEventEnvelopeBytes`, `abi::kCmdProducedSequence`, `kWireRevision = 2` | `abi/wire.h` | |
| **removed:** `core::module::UnmapPolicy`, `BackendModule::backendConstructed()` | | the pin-forever policy the review rejected |

Test-only environment variable: `CLASHQT_FAKE_MODULE_UNPINNABLE` makes the
DOUBLE present a quiescent-but-unmappable module, so the loader's honouring of
`kFalse` is proved rather than assumed. It can only move the answer downwards.

---

## 4. Evidence

* `abi-contract-tests`: **35 passed, 0 failed, 0 skipped**, Debug and Release.
  Eight cases are new: the activated-module unmap, the honoured refusal, the
  retained root reference, the retained reply buffer (ordinary and
  test-control), the retained diagnostic and its nested message buffer, the
  parallel create/prepare race, and the two direct-vs-module observer
  comparisons.
* `connection-snapshot-marshal-tests`: **15 passed** in Release, packed 412.67 ms
  / 98 182 B vs naive 460.33 ms / 127 336 B. In **Debug** the ratio assertion
  fails (1245 ms vs 1233 ms) — a build-type artifact of an unchanged codec, not
  a regression from this package; the same file fails the same way before these
  changes. Worth a `CMAKE_BUILD_TYPE` guard, which is the coordinator's call.
* Sample consumer, real source-built engine, exit 0, image verified unmapped.
* Mutation testing, external copy only (`inversions.py`): see below.

### Mutation testing

Sixteen inversions, **all sixteen detected**, each patching the snapshot, rebuilding, running the suite
that should notice, and reverting. Detected failures are recorded in
`inversions.jsonl`.

The runtime-pinning inversion is run against the SAMPLE and the real engine, not
the unit suite, because that is the only place the defect is reachable: making
`pinDependencies` a no-op that still reports success reproduces the original
**SIGSEGV (exit -11)** on demand. That is the strongest single piece of evidence
here — the fix is load-bearing, and removing it brings the crash straight back.

---

## 5. Honest limits

* **Not compiled in the repository tree.** External snapshot only, per the
  lease.
* **macOS arm64 only.** Darwin 25.6.0, AppleClang 17, Qt 6.11.1. The Linux and
  Windows pinning paths compile but were never run, and nothing here claims
  otherwise.
* **The pinning is coarse by design.** It pins every direct dependency, not only
  the ones known to register process-global state, because a module cannot know
  which of its dependencies do. yaml-cpp and the system frameworks are pinned
  too; that is a leaked mapping the process would have kept anyway.
* **`kCmdProducedSequence` is one synchronous command per `addObserver`.**
  Registration is a human-rate event, so this is not on any hot path, but it is
  a call into the module from a place that did not previously make one.
* **A module that supplies no sequence accessors reports 0**, and a host that
  sees 0 admits everything — the pre-envelope behaviour. That degradation is
  documented in wire.h rather than being silent, but it is a degradation.
