# COMPONENT-ABI — build registrations, dependencies and proofs

Worker: COMPONENT-ABI (Opus). Package round 2, implementation.
Qualification: **macOS arm64 only** (Darwin 25.6.0, AppleClang 17, Qt 6.11.1,
CMake 4.4.2, Ninja). Windows and Linux are compiled-for but **unqualified**:
no runner was available and none of the claims below was measured there.

Everything here was built and run in an external snapshot at
`/tmp/clash-qt-p4.w0Y8Uo/COMPONENT-ABI/snap` with its own standalone
`CMakeLists.txt`. **No repository build tree was touched.**

---

## 1. Source lists, by target

The coordinator's `tests/contracts/component/abi/CMakeLists.txt` already names
`clash_component_marshal`, `clash_module_host`, `clash_qt_backend_module` and
`clash_qt_fake_backend_module`. These are the sources each needs.

### `clashqt_backend_abi` — INTERFACE, published headers, no sources

```
src/core/component/abi/abi.h
src/core/component/abi/backend_abi.h
src/core/component/abi/module_entry.h
src/core/component/abi/target_abi.h
src/core/component/abi/wire.h
```

Links `clashqt_com` **only**. Must not acquire `Qt6::Core`: these headers
include no Qt header, and that is the property the sample project proves.
`AUTOMOC OFF`, like `clashqt_com`.

### `clash_component_marshal` — STATIC, private, compiled into BOTH sides

```
src/integrations/component/marshal/codec.cpp
src/integrations/component/marshal/com_objects.cpp
src/integrations/component/marshal/backend_marshal.cpp
src/integrations/component/marshal/connection_snapshot.cpp
src/integrations/component/marshal/runtime_tag.cpp
```

- `PUBLIC` include dir `${PROJECT_SOURCE_DIR}/src`
- `PUBLIC clash_backend clashqt_com Qt6::Core` (plus `clashqt_backend_abi`)
- `POSITION_INDEPENDENT_CODE ON` — it is linked into a `SHARED` library
- `AUTOMOC OFF` (no `Q_OBJECT` anywhere in it)

### module-side adapter sources (no separate target needed)

Compiled into **each** module target, as the coordinator's file already does
for the fake:

```
src/core/mihomo/module/backend_session.cpp
src/core/mihomo/module/backend_module.cpp
src/core/mihomo/module/host_privileged_service.cpp
src/core/mihomo/module/module_export.cpp
```

`AUTOMOC OFF`. They need `clash_component_marshal`, `clash_backend`,
`clashqt_com`, `clashqt_backend_abi`.

### `clash_qt_backend_module` — SHARED (the shipping module)

```
src/core/mihomo/module/mihomo_module_entry.cpp   + the four adapter sources
```

- `PRIVATE clash_component_marshal clash_mihomo_impl`
- `CXX_VISIBILITY_PRESET hidden`, `VISIBILITY_INLINES_HIDDEN ON`
- output directory `${CMAKE_BINARY_DIR}/modules` (matches the coordinator's file)

### `clash_qt_fake_backend_module` — SHARED, tests only

```
tests/support/component/fake_module_entry.cpp    + the four adapter sources
```

- `PRIVATE clash_component_marshal clash_backend_fake`
- same visibility and output directory

### `clash_module_host` — STATIC (the host shim)

```
src/integrations/component/module_loader.cpp
src/integrations/component/module_backend.cpp
```

- `PUBLIC clash_component_marshal clash_backend clashqt_com clashqt_backend_abi Qt6::Core`
- `AUTOMOC OFF` — `ModuleBackend` is **not** a `QObject` (it holds a plain
  `QObject pump_` for a queued drain). The Qt-native view stays `BackendBridge`'s
  job, unchanged.
- On Linux it needs `${CMAKE_DL_LIBS}`; on macOS `dlopen` is in libSystem.

### suites

`abi-contract-tests` and `connection-snapshot-marshal-tests` — already
registered by the coordinator, and the environment variables it sets
(`CLASH_QT_BACKEND_MODULE`, `CLASH_QT_FAKE_MODULE`) are exactly the ones
`tests/support/component/module_fixture.h` reads, in that order, before falling
back to the compile definitions `CLASHQT_BACKEND_MODULE_PATH` /
`CLASHQT_FAKE_MODULE_PATH`. Nothing further is needed.

---

## 2. Two registration findings the coordinator has to act on

### 2a. Hidden visibility must be set on the STATIC libraries too, not only on the module

`CXX_VISIBILITY_PRESET hidden` on a `SHARED` target applies to **that target's
own sources**. Measured in the snapshot:

| build | exported symbols in `libclash_qt_backend_module.dylib` |
| --- | --- |
| hidden on the module only | **2237** |
| hidden also on `clashqt_com`, `clash_component_marshal`, the adapter sources, `clash_mihomo_impl`, `clash_yaml` | **1** (`_clashqt_component_module_entry`) |

`nm -gU` both times. Until the static libraries carry the property, every
internal symbol of `CoreProcess`, `MihomoClient` and the marshalling leaves the
module — which is the opposite of "public ABI only". The fake module measures
the same way, also 1.

### 2b. `examples/component_consumer/` has no build file, by instruction

`main.cpp` and `README.md` only. It must build **outside** the main graph
(module-r1). The README carries a verified by-hand command; it compiles and
links against `Qt6::Core` and the platform loader alone. Verified:

```
c++ -std=gnu++20 -O1 -o component_consumer examples/component_consumer/main.cpp \
    -I src -isystem /opt/homebrew/lib/QtCore.framework/Headers \
    -F/opt/homebrew/lib -framework QtCore        # exit 0
```

---

## 3. Public API names introduced

| Name | Header | What it is |
| --- | --- | --- |
| `clashqt::com::abi::IBackendHost` | `abi/backend_abi.h` | host-implemented: `Notify`, `Invoke` |
| `clashqt::com::abi::IBackendSession` | `abi/backend_abi.h` | module-implemented: `SetHost`, `Invoke`, `LastError`, `Close`, `OutstandingWork` |
| `clashqt::com::abi::IModuleLifetime` | `abi/backend_abi.h` | `LiveObjectCount`, `PrepareUnload` |
| `kBackendSessionClassId`, `kMihomoModuleId`, `kFakeModuleId` | `abi/backend_abi.h` | fresh owned ids |
| `Command`, `HostCommand`, `Event`, `CloseFlags` | `abi/wire.h` | the versioned protocol; `kCmdTestControlBase = 0xF0000000` |
| `ConnectionSnapshotHeader`, `ConnectionRecord`, `TextRef` | `abi/wire.h` | the packed telemetry layout |
| `ModuleHandshakeRequest/Response`, `ModuleEntryFn`, `CheckHandshake`, `MakeHandshakeRequest`, `CLASHQT_COM_MODULE_ENTRY_NAME` | `abi/module_entry.h` | the handshake |
| `kTargetAbiTag`, `kTargetIsQualified` | `abi/target_abi.h` | compile-time target identity |
| `clashqt::integration::ModuleLoader` | `integrations/component/module_loader.h` | explicit-path ctor; `installedModulePath()`; `load`, `createSession`, `unload`, `isMapped`, `liveObjectCount`, `handshake`, `lastError` |
| `clashqt::integration::ModuleBackend` | `integrations/component/module_backend.h` | `core::backend::MihomoBackend` + `isValid`, `lastError`, `drain`, `shutdown`, `outstandingWork`, `isInsideCommand`, `queuedEventCount`, `unknownEventCount` |
| `core::module::UnmapPolicy` | `core/mihomo/module/backend_module.h` | per-module, set by its entry point |

Interface ids (freshly generated, permanently reserved):

```
IBackendHost      4b6abab7-235e-4876-9d53-5a96686b91c5
IBackendSession   296b1610-e991-4136-adbf-4a25c4136ef8
IModuleLifetime   ddf41729-c7b1-44c7-bf96-e45d0e3f6802
BackendSession class  c131b23b-9d15-4a5d-83bf-a5aefba239a8
mihomo module id      75838d74-f2b7-4030-96ad-26d8ff05919d
fake module id        fb6a96db-6c97-4aa7-9092-b11b82f0f96c
```

---

## 4. Evidence

Every run used an isolated `CLASH_QT_DATA_DIR`, `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`. `com.clash-qt.clash-qt.plist` hashed before and
after **every** run: `681784383d6e...78371c` throughout, unchanged.

- `abi-contract-tests`: **28 cases, 28 passed**, 0 skipped.
- `connection-snapshot-marshal-tests`: **15 passed** (5 round-trip sizes, the
  interning size assertion, the ratio measurement, 6 `QBENCHMARK` rows).
- Sample consumer against the **source-built** mihomo
  (`build-core-probe/mihomo`, the provenance-recorded artifact) on loopback
  `127.0.0.1:29141`, `mixed-port: 0`: handshake → session → start → ready →
  stop **confirmed** → live objects 0 → exit 0.
- Both module artifacts export exactly one symbol.

### Measurement asked for by D8

500-row snapshot, 200 encode+decode round trips, Release, macOS arm64:

| | time | bytes per snapshot |
| --- | --- | --- |
| packed layout | 451 ms | 98 182 |
| naive per-field encoding, same data | 499 ms | 127 336 |

Per-snapshot at 500 rows: pack 1.56 ms, unpack 0.52 ms (Debug: 4.0 / 1.3 ms).
**The honest reading:** the packed layout is ~10 % faster and ~23 % smaller. It
is not the order-of-magnitude win the phrase "packed buffer" suggests; what D8
asked to be measured was that it is **not a regression**, and it is not. The
suite asserts the ratio against the naive encoder in the same process, never an
absolute budget.

### Mutation testing — 33 inversions, 31 killed

Run on the external copy only (`inversions.py` in the scratch directory patches
`snap/`, rebuilds, runs both suites, reverts). Killed, with the case that
caught each:

handshake: abi version, wire revision, target tag, runtime tag, struct size,
identity, and "writes a module on refusal" (7) · loader: empty path, platform
search (2) · lifetime: always permits unload, never decrements, hands out
objects after prepare, pinned module reports itself unmappable, loader ignores
the pinned answer, the double claims to be pinned, session never reports
activation (7) · session: closed session still serves commands, close reports
success on timeout, outstanding work always zero, ignores trailing argument
bytes, shipping module installs a test extension (5) · host: ignores removal
during delivery, detaches the listener twice (2) · codec: request ids narrowed
to 32 bits, invalid timestamp becomes the epoch, privileged status drops
`coreRunning`, reader does not latch an overrun (4) · snapshot: skips field
bounds, skips record size, tolerates trailing bytes, skips chain-run
validation, stops interning (5).

**Two survivors, both recorded in the code with their justification:**

1. `host-delivers-events-inside-a-command` — deleting the `commandDepth_ > 0`
   term of the delivery guard fails nothing. Both modules in this tree wrap a
   backend that already queues its callbacks, so no `Notify` ever arrives
   inside a command. It is defence in depth for a module that delivers
   synchronously, which the ABI permits and D8 explicitly warns about.
2. `host-leaves-the-listener-attached` — deleting `ModuleBackend::shutdown`'s
   fallback detach fails nothing, because the shipping module's `CoreProcess`
   detaches itself and the double never attaches. It is the half of the seam
   that does not depend on the module behaving.

Both are redundancies **for the modules that exist today**, not coverage gaps
in the behaviour that is claimed. Two earlier survivors were real gaps and were
closed with new cases (`aPreparedModuleCreatesNothingFurther`,
`theReaderLatchesAnOverrunInsteadOfContinuing`).

---

## 5. The defect the sample found, and what changed because of it

The first sample called `dlclose` unconditionally. Driving the **real** engine
to readiness and back, it segfaulted in `dlclose` **every time** (exit 139),
after printing `live objects before unload: 0`. The identical call on a module
that had never been given a host returned cleanly — which is why the ABI suite
had not caught it: it never activated the shipping module before unmapping.

Cause: a module that constructs `QObject`-derived collaborators registers
meta-objects with a `QCoreApplication` that outlives the module, and unmapping
its image leaves those registries pointing into unmapped memory.

`IModuleLifetime::PrepareUnload` now answers **`kFalse`** — component-r1's
"succeeded, and the answer is negative" — for a module that is quiescent but
pinned. `ModuleLoader::unload()` releases the root and keeps the image mapped,
and `isMapped()` publishes which happened. The policy is **per module**: the
test double declares itself `Unmappable` and is genuinely unmapped, and
`anUnmappableModuleIsUnmappedAndAPinnedOneIsNot` pins both arms. This is not
"never unload" — a module loaded and never activated is unmapped, including the
shipping one.

**For the coordinator:** hot-swapping the shipping module inside a running
application is therefore not available on this evidence. Replacing it across a
restart is. If hot-swap becomes a requirement, the next step is to find the
specific registries involved, not to remove the guard.

---

## 6. Honest limits

- **Not yet compiled in the repository tree.** Everything was built in the
  snapshot against copies. The coordinator's `CMakeLists.txt` names targets that
  match the source lists above, but the in-tree configure is unverified by me.
- **`backend-r4` semantics are not re-asserted here.** This suite tests the
  boundary. Running the existing `backend-contract` suite against a
  module-backed fixture is the next round's assignment, and
  `ModuleFixture::contractBackend()` exists for exactly that.
- **`OutstandingWork()` tracks only the operations backend-r4 documents as
  settling with a completion.** Streams, `closeConnection` and `attach`/`detach`
  return an id for correlation and settle with no completion carrying it, so
  counting them would make `Close` time out every time. Named in the code with
  the reason.
- **Windows/Linux unverified.** The loader has `LoadLibrary`/`GetProcAddress`
  paths and the target tag distinguishes the MSVC C++ ABI, but neither was
  compiled or run.
- **The `kAnyModuleId` wildcard is a harness affordance**, not a production
  path. The shipping loader's default is `kMihomoModuleId`.

---

## 7. Reconciliation with `cmake/Component.cmake` as it stands

Read after writing the above; the coordinator's file already matches the source
lists, target names and output directory. Three notes, in descending order of
importance:

1. **`clashqt_com` and `clash_yaml` still leak their symbols into the module.**
   Both are `STATIC` targets without `CXX_VISIBILITY_PRESET hidden`, and both
   are linked into `clash_qt_backend_module` (the first transitively through
   `clash_component_abi`). `clash_component_marshal` and the directly-compiled
   mihomo sources are already covered. Measured in the snapshot: with every
   static contributor hidden the module exports exactly **1** symbol; without,
   **2237**. `clashqt_com` contributes only `FormatInterfaceId` and
   `ResolveInterface`, so the residue is small — but "public ABI only" is a
   property worth having exactly, and it is two properties away.
2. **`clash_qt_backend_module` needs `AUTOMOC ON`**, which it gets by default
   from `qt_standard_project_setup`. Flagged only because `clashqt_com` and
   `clash_component_marshal` both opt out explicitly nearby, and a future edit
   that copies that pattern onto this target would break it: `CoreProcess`,
   `MihomoClient` and `ProviderClient` all carry `Q_OBJECT`.
3. **`install(DIRECTORY src/core/component/ ...)` already ships `abi/`.** That
   is the header set `examples/component_consumer` compiles against, and it is
   sufficient on its own — verified by compiling the sample with `-I src` and
   `Qt6::Core` alone.
