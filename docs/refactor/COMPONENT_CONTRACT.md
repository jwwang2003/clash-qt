# Portable component object model — contract revision `component-r1`

Coordinator-owned. This is the published semantic contract that `src/core/component/`
must implement and that every component consumer codes against. A worker implements
these primitives under an explicit lease; it does not redesign them.

Implements G2 and the COMPONENT-BASE section of `docs/COM_MODULE_PLAN.md`.

## Provenance and the reference audit

`3rdparty/ref/fxcom` is a **read-only design reference**. It carries no licence file,
so no implementation is copied from it. This contract is an independent design of the
reviewed object model. The reference tree is never modified and never built.

Three defects in the reference are deliberately not reproduced:

| Reference defect | Evidence | This contract |
| --- | --- | --- |
| Failure detection is inoperative | `Types.h`: `using FRESULT = uint32_t;` with `#define FFAILED(hr) (((fx::FRESULT)(hr)) < 0)`. An unsigned value is never `< 0`, so `FFAILED` is always false and `FSUCCEEDED` always true. The negative constants (`FE_NOINTERFACE = -2` …) wrap to large positives. | `Result` is `int32_t`. Negative is failure. `IsFailure()`/`IsSuccess()` are inline functions, not macros. Every expected-failure test must observe a genuine failure. |
| Base interface ID is all zeroes | `FX_IID(IObject, "00000000-0000-0000-0000-000000000000")` | Every interface, including the base, has a freshly generated owned ID. |
| Duplicate error code | `FE_INVALID_ARGS = -4` and `FE_NOT_ALIVE_OBJECT = -4` are the same value, so the two conditions are indistinguishable. | Every code is distinct and its meaning is tested. |

No `FX`/`fx` prefix, namespace, header path, target name or user-facing label appears
in the published API. The only permitted mention of the reference is attribution.

## Names

| Concept | This project |
| --- | --- |
| Namespace | `clashqt::com` |
| CMake target | `clashqt_com` |
| Include root | `core/component/…` (project-rooted, `src` is the include root) |
| Identity | `InterfaceId` |
| Status | `Result` |
| Base interface | `IObject` |
| Owning pointer | `ComPtr<T>` |

## Owned interface identifiers

Freshly generated, permanently reserved. An ID names an **immutable** vtable: once
published, method order and signatures never change. An incompatible interface gets a
new ID; both may be exposed through `QueryInterface` during a migration.

| Interface | Id | Revision |
| --- | --- | --- |
| `IObject` | `247a1b90-ece9-43a9-ab82-5739bdff6445` | r1 |
| `IComponentModule` | `66fdef80-2cd3-4808-ac33-547172c2952f` | r1 |
| `IErrorInfo` | `e83b4037-e096-4ae8-8f94-0f56ad29568a` | r1 |
| `IBuffer` | `8fdacf5e-be9a-47e0-bb57-1375a2322e54` | r1 |
| `IWeakReference` | `5fd359f7-579a-4bcf-9743-cbbb8c5da432` | reserved, not implemented in r1 |
| `IWeakSource` | `558f7604-facb-4122-86a8-16f66d58eac2` | reserved, not implemented in r1 |

Weak references are reserved but **not built in r1**. They are added only when a real
consumer needs them, together with resolution/destruction race tests and control-block
lifetime tests. Reserving the ID now prevents a later collision; it promises nothing.

## `Result`

```
using Result = std::int32_t;
constexpr bool IsSuccess(Result r) noexcept { return r >= 0; }
constexpr bool IsFailure(Result r) noexcept { return r <  0; }
```

| Code | Value | Meaning |
| --- | --- | --- |
| `kOk` | `0` | Operation succeeded. |
| `kFalse` | `1` | Succeeded, and the answer is negative (distinct from `kOk`, still success). |
| `kFail` | `-1` | Unspecified failure. |
| `kNoInterface` | `-2` | The object does not implement the requested id. |
| `kNotImplemented` | `-3` | Method exists in the vtable but is not implemented by this object. |
| `kInvalidArgument` | `-4` | A caller-supplied argument is unusable. |
| `kNotFound` | `-5` | A named entity does not exist. |
| `kTimeout` | `-6` | A bounded wait expired. |
| `kCancelled` | `-7` | The operation was cancelled before completion. |
| `kUnsupportedVersion` | `-8` | ABI or interface version negotiation failed. |
| `kInvalidState` | `-9` | The object is not in a state that permits this call. |
| `kAlreadyClosed` | `-10` | The object has been closed or drained. |

Values are stable. New codes append. `kFalse` exists so that a successful "no" is not
reported as a failure — the reference has no such distinction.

## `IObject`

```
struct IObject {
    virtual Result QueryInterface(const InterfaceId& id, void** out) noexcept = 0;
    virtual std::int32_t AddRef() noexcept = 0;
    virtual std::int32_t Release() noexcept = 0;
};
```

Exact required behaviour — these are the assertions the contract tests must make:

1. **`out` is null** → return `kInvalidArgument`. Nothing is written, no reference is
   taken. (The reference leaves this unspecified.)
2. **Unsupported id** → write `nullptr` to `*out` **and** return `kNoInterface`. The
   write happens before the return so a caller that ignores the code cannot read an
   uninitialised pointer.
3. **Supported id** → write a non-null pointer and return `kOk`, having taken
   **exactly one** new strong reference. The caller owns that reference and must
   `Release()` it. A failed query never changes the reference count.
4. **Identity.** Querying `IObject` on any interface of one object yields the same
   pointer value. Querying is reflexive (`A`→`A`), symmetric (`A`→`B`→`A` returns the
   original `IObject` identity) and stable for the object's lifetime: an id that
   succeeds once succeeds for as long as the object lives, and one that fails always
   fails. Multiple inheritance means *interface* pointers may legitimately differ;
   only the `IObject` pointer is the identity.
5. **`AddRef`/`Release`** are atomic and need no external synchronisation. They return
   the count after the operation; only a returned `0` from `Release` is reliable, and
   it means the object was destroyed. Calling any method after that is undefined.
6. **Atomic refcounting is not method thread-safety.** Each interface documents its own
   thread affinity separately. r1 objects are callable from any thread only where the
   interface says so; otherwise calls belong to the thread that created the object.
7. **Destruction happens in the allocating module.** `Release` reaching zero runs the
   destructor and frees the storage inside the module that constructed it. Memory is
   never freed across a module boundary by the consumer's allocator.

## `ComPtr<T>`

Intrusive owning pointer. The two ways to take ownership are explicit and cannot be
confused at a call site:

- `ComPtr<T>::Adopt(p)` — takes an **already-owned** reference; does not `AddRef`.
  This is what `QueryInterface` and every factory output feeds.
- `ComPtr<T>::Retain(p)` — takes a **borrowed** pointer; calls `AddRef`.

Copy retains, move transfers, destruction releases, reassignment releases the old value
after acquiring the new one (self-assignment safe). `Put()`/`GetAddressOf()` returns a
`T**` for output parameters and releases any previously held value first, so a pointer
cannot leak by being overwritten. `Detach()` yields ownership to the caller. There is
no implicit conversion to the raw pointer.

## `IComponentModule`, `IErrorInfo`, `IBuffer`

`IComponentModule` is the queryable root a loaded module hands back: module identity,
module ABI version (distinct from individual interface versions), a human-readable
description, and `CreateObject(classId, interfaceId, out)`. The **exported C factory
entry and the host/module handshake belong to COMPONENT-ABI**, not to r1; r1 defines
only the interface that entry will return.

`IErrorInfo` carries an optional diagnostic alongside a failure `Result`: a stable code,
a UTF-8 message and an optional source tag. It is retrieved from the failing object,
never through thread-local global state. Absence of error info is normal and is not a
failure.

`IBuffer` is the owned byte range used wherever data crosses a module boundary: size,
read pointer, writable pointer and a resize that can fail. The **producing module owns
and frees the storage**; the consumer holds a reference and releases it. This is why no
`std::string`, `QByteArray`, `QString` or container ever appears in a published
signature.

## What r1 deliberately excludes

Pulled in only when a real consumer needs them, each with its own tests: weak references
and delegation, a replaceable memory allocator, lock-free collections, signal/event
primitives, threading utilities, IPC or marshalling, and any Microsoft COM integration.
The reference ships several of these; their presence there is not a reason to import them.

`QueryInterface` plus a C factory does **not** by itself make a C++ vtable portable
across compilers and runtimes. Every supported target ABI is certified explicitly by
COMPONENT-ABI. r1 claims a C++ object model, not binary compatibility.

## Acceptance for COMPONENT-BASE

`tests/contracts/component/` must cover, as genuinely failing-when-broken cases: null
`out`; unknown id returning `kNoInterface` **with** a nulled output; exactly-one-reference
on success; unchanged count on failure; query identity/reflexivity/symmetry/stability;
`Adopt` vs `Retain` counts; `ComPtr` self-assignment, move, and overwrite-without-leak;
destruction at zero; every `Result` code distinct; and `IsFailure` actually returning
true for every negative code — the specific defect the reference demonstrates.

A test that passes against a deliberately broken implementation is not coverage. Each
contract case is validated by temporarily inverting the behaviour it protects.
