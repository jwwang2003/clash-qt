# Portable COM component implementation plan

Status: proposed, updated 2026-09-21. Implements [G2](PROJECT_GOALS.md) and the
[parallel roadmap](IMPLEMENTATION_ROADMAP.md). The user-selected reference is
`3rdparty/ref/fxcom`; only its COM object model is relevant. This supersedes the
previous assumption that native Microsoft COM was required on Windows.

## Decision and reference findings

Use one project-owned portable COM-style component model on macOS, Windows and
Linux. Adopt interface identity/query, explicit reference ownership and component
boundaries. Do not adopt FX/fx prefixes, unrelated project branding, threading
utilities or an entire framework merely because it exists in the reference.

The reference supplies:

| Local source | Relevant design |
| --- | --- |
| [Types.h](../3rdparty/ref/fxcom/include/fx/core/object/Types.h) | GUIDs, IObject query/retain/release, weak-reference contracts |
| [Foundation.h](../3rdparty/ref/fxcom/include/fx/core/object/Foundation.h) and [Foundation.cpp](../3rdparty/ref/fxcom/src/core/object/Foundation.cpp) | Object implementations, interface tables, atomic reference counts and construction/delegation |
| [AutoPtr.h](../3rdparty/ref/fxcom/include/fx/core/object/AutoPtr.h) | Intrusive smart-pointer ownership patterns |
| [CMakeLists.txt](../3rdparty/ref/fxcom/CMakeLists.txt) | A static C++17 foundation library, not an external module loader |
| [Reference tests](../3rdparty/ref/fxcom/tests/src/core/RefCntAutoPtrTest.cpp) | Lifetime, weak-reference, construction and concurrent-reference examples; not run during planning |

This reference does not provide shared-library activation, module versioning,
process IPC, IDL/marshaling, or Microsoft COM runtime integration. Its constructor
helpers are not a plugin loader. None of these additional capabilities may be
claimed simply because the object interfaces look like COM.

Audit and adapt instead of renaming the code wholesale. In the reviewed Types.h,
FRESULT is unsigned while FFAILED tests for a negative value: failure detection is
broken. The base IID is all zero; new interfaces need new project-owned IDs. The
query helper also needs a specified null-output contract. Keep the reference tree
unchanged. No license file was found there; establish permission/provenance before
copying implementation, or implement the reviewed design independently. Preserve
required attribution if implementation reuse is subsequently authorized.

## Names and boundaries

Proposed namespace: `clashqt::com`; CMake target: `clashqt_com`. Public names include
`InterfaceId`, `Result`, `IObject`, `ComPtr`, and optional `IWeakReference`/`WeakPtr`.
Use new IDs. No FX/fx-derived API prefixes, namespaces, target names, include paths
or user-facing labels. The original path/name remains only in reference attribution.

```text
clash-qt application services
  -> portable COM interfaces / module loader
     -> Mihomo module -> locally built mihomo executable
     -> Monitor module -> host collector + containerized monitor backend
     -> Mitmproxy module -> supervised Python/mitmdump worker
```

Object pointers never cross into Go, Python or a container. C++ adapters implement
the interfaces and use explicit bounded process protocols internally. Privileged
operations stay in narrow OS helpers. Refcounts do not confer privilege or make
arbitrary methods thread-safe. This boundary allows later native Microsoft COM
interoperability, but that is an optional separately tested extension.

## COMPONENT-BASE — Foundation before backend extraction

Coordinator owns published headers in `src/core/component/`. A bounded worker
implements reviewed primitives in that directory and corresponding
`tests/contracts/component/` files under an explicit lease before P3 consumers start.

Define stable interface identity, QueryInterface success/failure behavior, exactly
one returned strong reference on success, null output on unsupported IID, invalid
argument behavior, retain/adopt/release semantics, and errors with unambiguous signed
or explicit success tests. Destruction happens in the allocating module. Document
thread affinity of calls independently from atomic reference operations.

Implement only needed foundations first. Weak references/delegation are added when
an actual consumer requires them, with lifetime/race tests. Avoid importing allocator,
lock-free collections, or signal primitives unrelated to the module boundary.

## COMPONENT-ABI — Shared-library factory and loader

Create a versioned exported C factory entry plus queryable component interfaces,
with a host/module compatibility handshake. Define calling conventions, layout,
fixed-size fields, string encoding, buffer ownership/freeing, error propagation and
supported compiler/runtime combinations. Keep Qt/STL values and C++ exceptions
inside implementation boundaries. The C factory alone does not make every C++
interface ABI portable; certify each supported target ABI explicitly.
Published interface vtables are immutable: never change method order/signatures
under an existing IID. Add a new IID for an incompatible interface and expose
supported versions through QueryInterface; distinguish module ABI version from
individual interface versions. Test old consumers against compatible new modules.

Loader implementation lives under `src/integrations/component/`. Load only selected
installed module artifacts, validate identity/version before activation, and report
missing/incompatible components. Keep the library loaded while any object, callback,
operation or weak-reference control block can execute its code. Use drain/cancel/close
before unloading; do not infer asynchronous stop completion from a final Release.

Acceptance: independently built sample consumer and fake module load/query/operate/
close on macOS, Windows and Linux; incompatible versions fail predictably; no FX
naming escapes into the published API. A static-library-only test is insufficient.

## MOD-CORE — Mihomo implementation

Build `src/core/mihomo/mihomo_backend.*` in a separately packaged module by adapting
CoreProcess, MihomoClient and ProviderClient. Build its engine from `3rdparty/mihomo`
as required by G1. The app links only contracts/loader, not implementation objects.

Expose lifecycle, configuration, control, telemetry and capabilities through focused
interfaces. Specify request IDs, accepted versus completed operations, cancellation,
deadlines, desired/applied revision and endpoint generation. Managed and externally
attached cores have different ownership; detach never terminates an external core.

Test ordinary profile validation/start, controller query, local proxy traffic,
configuration change, stop and recovery against the packaged source-built engine.
Authority-side policy checks cover all component mutation entry points when managed
mode is implemented. A component interface is not a tamper-proof security boundary.

## COMPONENT-PACKAGE — Distribution and qualification

Ship a dylib/DLL/so component plus its engine/runtime dependencies on the matching
platform. Record source/component/core/ABI versions and verify installation-relative
paths without developer SDKs or source-tree paths. The Windows baseline uses the
same portable loader; no registry/type-library/COM activation dependency is required.

The monitor and MITM modules use this same foundation and publish narrow capture
session/provider contracts described in [CAPTURE_MODULE_PLAN.md](CAPTURE_MODULE_PLAN.md).
Python/collector/container dependencies can be optional installable add-ons; their
absence cannot prevent ordinary proxy use.

## Tests and optional Windows bridge

Test interface identity and supported query paths, unknown IDs, null-output/error
behavior, reference transfer, construction failure, method thread affinity and
cancellation. When weak references exist, cover resolution/destruction races and
library lifetime. Add module version rejection, live-object unload prevention,
separate-consumer compatibility, real-engine workflows and installed-artifact tests.
Expected failures must actually fail, addressing the reference's result-code issue.

A future native Microsoft COM bridge may implement IUnknown/IID, IDL, marshaling,
apartment and registration requirements around the same portable module. It needs
its own Windows runner and lifecycle/security tests. It is not a prerequisite for
G2, a substitute for the portable ABI, or part of the present required COM scope.
