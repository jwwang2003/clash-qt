# Architecture

This describes clash-qt as it is built today, not as it was planned. Every
claim here is meant to be checkable against `CMakeLists.txt`, the module graph
in `tests/architecture/architecture.json`, or the headers named in the text. If
one of them disagrees with this file, the code is right and this file is wrong.

The short version: clash-qt is a Qt Widgets desktop application that manages a
mihomo engine. The engine is not linked into the application. It lives in a
separately built shared module that the application loads at run time across a C
ABI, and the application talks to it through a published, Qt-typed contract that
both the real engine and a deterministic fake implement.

## Layers

Source lives in six top-level directories under `src/`, and the direction of
every dependency between them is fixed.

| Directory | Owns | May depend on |
| --- | --- | --- |
| `src/core/` | Engine implementation, configuration, profiles, backups, telemetry, preferences, the published backend contract, the component object model | Shared value types and Qt non-desktop modules |
| `src/platform/` | Operating-system adapters: system proxy, browser launch, autostart, global hotkeys, the privileged-helper client | Shared value types and the OS |
| `src/integrations/` | The host side of the module boundary: the loader, the host-side backend facade, the private marshalling codecs | The ABI headers and the backend contract |
| `src/app/` | Application services lifted out of the entry point: reload scheduling, routing intent, the quit gate, backup ownership, composition-root adapters | The backend contract, platform, profiles |
| `src/ui/` | Widgets: the shell, the pages, shared controls, theme | Everything below it, through published contracts only |
| `src/services/` | The standalone privileged helper executable (macOS) | Native frameworks and its own IPC contract |

`src/main.cpp` is the composition root and sits above all of them.

Two rules make the layering real rather than aspirational, and both are checked
mechanically on every run (see [testing.md](testing.md)):

- **Backend code never reaches up.** Nothing under `src/core/` or
  `src/platform/` may include `ui/**` or `app/**`, and nothing there may use Qt
  Widgets, Quick or Graphs. Qt Gui and Qt Qml are deliberately allowed: a
  platform adapter puts `QKeySequence` in a public header, and the profile
  enhancement scripts run on `QJSEngine`. Gui is not Widgets and Qml is not
  Quick.
- **Core and platform do not know about each other.** `src/platform/**` may not
  include `core/**` except the shared value types, and `src/core/**` may not
  include `platform/**` at all. When core code needs the operating system, it
  publishes an interface of its own and the composition root supplies the
  concrete platform object. That is exactly how the privileged-service seam
  works, and it is why `src/app/composition/` exists: it is the only place in
  the tree allowed to see both sides.

## The libraries

Each library owns one responsibility and declares only the dependencies its code
actually has. A target that links what it does not use makes the dependency
graph lie, and the architecture checker reads that graph.

**Foundations.** `clash_types` is header-only shared value types; everything may
depend on it and it depends on nothing but Qt Core. That is what lets the UI,
the configuration library and the component-private engine share types without
any of them depending on one another. `clash_yaml` is YAML helpers as their own
leaf, kept out of `clash_config` so the separately packaged component never
drags the configuration library into its package. `clash_preferences` is the one
accessor for the application's preference store, a leaf beside `clash_yaml`, so
the backup store, the UI and the composition root can each read a setting
without depending on one another.

**Configuration and data.** `clash_config_compose` is a pure YAML composer:
immutable input, no disk, no network, no process, no script engine.
`clash_config` adds the enhancement chain that runs profile scripts through
`QJSEngine`. `clash_profiles` is profile persistence and subscription refresh.
`clash_backups` is archive storage and WebDAV. `clash_telemetry` is bounded
traffic history — the narrowest module in the tree, and for that reason the
sharpest probe for a dependency arriving through some broad umbrella target.

**The engine.** `clash_mihomo_impl` is the engine implementation: the controller
client, the provider client, controller discovery and the supervised child
process. It is component-private. Nothing may link it except the handful of test
suites whose subject it *is*; everything else reaches the engine through the
published contract. This is enforced, not merely stated — see "The seal" in
[testing.md](testing.md).

**The published seam.** `clash_backend` is a header-only interface library: the
`core::backend::MihomoBackend` facade the application codes against, which both
the real engine and the deterministic fake implement. It is one object exposing
five facets — lifecycle, attachment, control, telemetry, capabilities — as
separate base classes with protected destructors, so a consumer that needs only
part of the surface takes a reference to the facet it needs and cannot reach the
rest. The contract's rules (request generations, observer admission by
production, non-re-entrant delivery, nothing unwinds) are stated in
[module-api.md](module-api.md).

`clash_backend_bridge` is the Qt-native view of that contract. The observer
interface is a plain non-`QObject`; around sixty-six UI call sites want signals.
The bridge is deliberately *not* itself an observer — twenty-three callbacks
share a name with their signal, which would make `&BackendBridge::proxiesUpdated`
ambiguous at every call site — so the sink is a nested member instead.

**The component object model.** `clashqt_com` links nothing at all, not even Qt
Core. See the next section for why.

**The module boundary.** `clash_component_abi` is the publishable ABI header set.
`clash_component_marshal` is the private codecs, compiled into both sides.
`clash_qt_backend_module` is the shipping shared artifact. `clash_module_host`
is the host's loader and facade. See "Loading the engine" below.

**Application services.** `clash_app_lifecycle` is the quit gate.
`clash_app_backup` owns the process's one backup store, so shutdown no longer
discovers it by walking the widget tree. `clash_app_runtime` is the reload gate,
snapshot retention and routing intent. `clash_app_composition` is the adapter
that joins the privileged seam's two sides.

**The UI is not a library.** It is compiled into the `clash-qt` executable from
per-directory source manifests. Extracting it waits until its test substitution
points are explicit, so that native resource linking stays verifiable at each
step. Widget suites therefore name the specific `.cpp` files they need rather
than linking a UI target.

## The portable component object model

`clashqt::com`, in `src/core/component/`, is a small COM-style object model
written for this project. It exists so that a module built separately — possibly
by a different compiler invocation, certainly with its own allocator — can hand
objects to the application without either side reaching into the other's memory.

It deliberately links nothing, not even Qt Core. A Qt type in a published
signature would defeat the entire point: `QString`'s layout and implicit sharing
depend on the exact Qt build, and its memory would be allocated in one module
and freed in another. The archive is verified to have zero undefined symbols.

The pieces:

- **`InterfaceId`** is a 128-bit identifier naming an interface, a class or a
  module. An id names an *immutable* vtable: once published, method order and
  signatures never change. An incompatible interface gets a new id, and both may
  be exposed through `QueryInterface` during a migration. Ids are built from a
  string literal at compile time; an invalid hex digit makes the expression
  non-constant, which is a compile error rather than a silently zero id.
- **`IObject`** is the base of every component interface, with exactly three
  methods: `QueryInterface`, `AddRef`, `Release`. Its destructor is protected
  and non-virtual, so `delete` on an `IObject*` does not compile — a consumer
  never frees an object with its own allocator. `Release` reaching zero destroys
  the object inside the module that constructed it. Reference counting is atomic
  and callable from any thread; that is a statement about the *count* only, and
  every interface documents its own thread affinity separately.
- **`QueryInterface`** has a fully specified contract, implemented once in
  `ResolveInterface` so it is not re-derived and re-broken in every class. A
  null out-parameter is `kInvalidArgument` with nothing written and the count
  untouched. An unsupported id writes `nullptr` *first* and then returns
  `kNoInterface`, so a caller that ignores the code cannot read an uninitialised
  pointer. A supported id yields exactly one new strong reference. Querying the
  base id on any interface of one object yields the same pointer: that pointer
  is the object's identity, and queries are reflexive, symmetric and stable for
  the object's lifetime.
- **`ComPtr<T>`** is the intrusive owning pointer, and the two ways to take
  ownership are *named* so they cannot be confused at a call site.
  `ComPtr<T>::Adopt(p)` takes an already-owned reference and does not
  `AddRef` — this is what `QueryInterface` and every factory output feeds.
  `ComPtr<T>::Retain(p)` takes a borrowed pointer and adds a reference of its
  own. There is no constructor from a raw pointer and no implicit conversion
  back to one; `Get()` is explicit about handing out a borrowed pointer. Copy
  assignment acquires the incoming reference before releasing the outgoing one,
  so self-assignment cannot destroy the object it is about to keep.
- **`Result` is signed.** This is the single most load-bearing decision in the
  file, and it is a correction rather than a preference. The design reference
  this model was assessed against declares its status type as an *unsigned*
  32-bit integer and then tests failure with `value < 0`, which is never true:
  every failure there reads as success and the negative constants wrap to large
  positive values. Keeping `Result` a signed 32-bit integer, keeping the
  predicates inline functions rather than macros, and giving every failure code
  a distinct negative value is what makes an expected-failure test able to
  observe a genuine failure. A static assertion in the header enforces the
  signedness, and more assertions enforce that `kOk` and `kFalse` are
  distinguishable and that no two failure codes collide.
- **`IBuffer`** is the owned byte range used wherever data crosses a module
  boundary. The producing module owns and frees the storage; a consumer holds a
  reference and releases it. That is why no `std::string`, `std::vector`,
  `QByteArray` or `QString` ever appears in a published signature.
- **`IComponentModule`** is the queryable root a loaded module hands back. It is
  free-threaded; objects it creates carry their own affinity.

Weak references are *not* implemented. Two ids are reserved for them by the
contract and deliberately left undeclared, because reserving an id promises
nothing and a weak reference will be built when a consumer actually needs one.

## Loading the engine

The engine runs behind a real binary boundary, not merely behind an abstract
class. `clash_qt_backend_module` is a shared library that compiles the engine
sources itself and is staged beside the application; on macOS it lands in the
bundle's `Frameworks` directory. The application does not link it.

**The handshake.** A module exports exactly one C symbol,
`clashqt_component_module_entry`. It is a C function with plain-old-data
arguments and a size-prefixed struct in each direction because it is the only
thing in the boundary that runs *before* the two sides have agreed they are
compatible — a C++ vtable, an exception or a `QString` at that point would
already be the undefined behaviour the handshake exists to prevent. What is
checked, in order, in a single shared function so both sides apply exactly the
same rules:

1. `structSize`, so the two sides agree about the shape of the structs before
   any field is read.
2. The module ABI version — the version of the handshake and of the wire
   protocol, distinct from any interface id's revision and from the backend
   contract's own interface revision.
3. The wire revision and the interface revision.
4. The target ABI tag: compiler, architecture, pointer width, endianness and
   C++ ABI flavour. A mismatch here is not a version problem, it is a different
   binary contract, and no version of either side would help.
5. The runtime tag — the shared Qt build the two sides must agree on, which no
   ABI header can see because no ABI header may include a Qt header. Host and
   module each compute it privately.
6. The module id. A loader that asked for the shipping module never accepts the
   test double, even though both export the same symbol.

On any failure the module writes `nullptr` and returns a failure code. It does
not construct anything and it does not try to work anyway; there is no fallback
in production. The response struct is filled in *always*, including on refusal,
so the loader can report what the module actually was rather than
"incompatible".

**The protocol.** Rather than sixty vtable slots, the boundary carries a
versioned command/event protocol over two three-method vtables. Sixty slots
would freeze the order of every operation, so adding one operation later would
require a new interface id and a migration for a change that is purely additive
in C++. A command code is data, so an unknown code is answered "not implemented"
by an older peer instead of dispatching into the wrong slot, and the vtables
stay three methods wide and immutable — which is what an interface id promises.

The encoding rules are stated exhaustively in `src/core/component/abi/wire.h`,
because a decoder that guesses is a crash: little-endian fixed widths, no
`size_t`, no bitfields, no padding assumptions; a string is a length prefix plus
UTF-8 bytes, never NUL-terminated and never assumed valid; a list is a count
plus that many elements, and a decoder that reads past its input fails rather
than clamping. Two details are worth knowing because they were bugs waiting to
happen:

- **Every event payload begins with the sequence number the module assigned when
  it *produced* the event.** The contract admits an observer by production, not
  by arrival. In process that costs nothing, because the producing queue and the
  observer list are the same object. Across a boundary they are not — the module
  produces and queues, the host receives later — so a host that stamped events
  on arrival would hand a newly registered observer events produced before it
  existed. The sequence travels so both sides apply the same rule.
- **A timestamp carries its representation, not just its instant.** A
  `QDateTime` is an instant *and* a time representation, and the host renders the
  second one. A value that arrives as local time where it left as UTC is a
  visible change in printed text even though equality — which compares
  instants — still succeeds.

The connection snapshot is the one exception to the general encoding: it is a
single packed buffer of fixed-layout records indexing a shared UTF-8 blob,
because it is the one payload large and frequent enough for per-string
allocation to matter. The packed layout was adopted against a measured cost, not
a promised speedup.

**The loader.** `ModuleLoader` takes either an explicit path the caller chose or
the installation-relative one. There is no source-tree fallback, no build-
directory fallback and no search of `PATH`: a loader that searches can load a
module the user did not install, and that failure mode is silent. Unloading is
accounting rather than a blanket refusal, and the protocol is shaped by a real
defect. A loader that simply dropped every reference and then looked at what was
left learned "somebody else is holding this" at the exact moment it had nothing
left to hold, so it could neither retry nor recover, and the image leaked for
the life of the process. The implementation instead releases its root interface
while keeping a lifetime interface, asks whether unload is safe while the object
is still pinned, and on refusal takes the root back and returns false with a
diagnostic. A refused unload changes nothing: the module is still loaded,
sessions still work, and the same call succeeds once the other holder releases.

**The host facade.** `ModuleBackend` is a `MihomoBackend` whose implementation
lives in the loaded module. This is the whole boundary in one class: the UI, the
Qt bridge and every `connect()` site are unchanged, because what they code
against is `MihomoBackend` and this *is* one. Command codes, packed buffers,
owned buffers and the reverse seam for privileged execution are all contained
here, so a boundary bug is one file's problem rather than sixty call sites'. The
facade re-implements the contract's own guarantees across the boundary: it
refuses to deliver an event while inside one of its own methods, drains in
arrival order, and honours removal-during-delivery.

**The reverse seam.** Privileged core execution runs the other way. The engine
publishes `core::PrivilegedCoreService`, an interface it owns, in the header-only
contract library rather than in its private implementation — precisely because
the composition root needs it. The composition root constructs the concrete
platform client and adapts it. Without that injection the backend falls back to
a null service whose `isSupported()` is false, service mode is silently
unavailable, and a user's saved preference is discarded without a word. That is
not hypothetical: it is a defect this project shipped once.

## The composition root

`src/main.cpp` does two things and nothing else: it constructs long-lived
objects and hands them to whoever needs them, and it owns the startup work for
which no service class exists — command line, single instance, settings
bootstrap. The reload gate is in `src/app/runtime/`, the routing intent is in
`src/app/runtime/`, the quit gate is in `src/app/lifecycle/` and the backup
rules are in `src/app/backup/`.

Construction order is declaration order, and destruction is its reverse. Each
line needs the one above it, and every coordinator is destroyed *before* the
backend it observes, which is what makes a pending single-shot restore, an
observer registration or a queued backend event impossible to outlive its
subject:

```
platform::PrivilegedServiceClient        the privileged seam's transport
core::PrivilegedServiceClientAdapter     adapts it to the engine's own seam
ModuleLoader / ModuleBackend             module lifetime and host facade
core::backend::BackendBridge             the Qt view of the backend
app::runtime::ProfileStoreConfigSource
app::runtime::RuntimeCoordinator         reload gate, snapshot retention
app::runtime::RoutingController          system proxy, TUN, mode
app::lifecycle::QuitGuard / ports / ShutdownCoordinator
app::backup::BackupCoordinator           owns the one backup store
ui::MainWindow, ui::TrayIcon             after every coordinator
```

The composition root is also where knowledge that belongs to *this process* is
supplied rather than discovered. The geo-data seed directory is the clearest
example: the profile store used to work the path out itself by calling into
controller discovery, and that one path string made the profiles library link
the component-private engine — which, because link edges propagate, handed every
consumer of profiles the private implementation and Qt WebSockets with it. The
composition root already knows which engine this process is managing, so it sets
the seed directory and the edge is gone rather than baselined.

Application services communicate with the shell by publishing state and
requests. The shell renders and opens dialogs. `ShutdownCoordinator` owns no
widget: it raises a warning, and the shell is what shows a message box and
reports the dismissal back, which is what holds the quit open until it is
acknowledged.

## Where a new feature belongs

Work down this list and stop at the first match.

1. **Does it talk to the mihomo engine?** Then it is an operation on the
   published backend contract, and it belongs in `src/core/backend/` as a
   contract change plus an implementation in `src/core/mihomo/`, a command code
   in the wire protocol and a case in the abi contract suite. Changing the
   contract means changing what [module-api.md](module-api.md) publishes,
   including its revision.
2. **Is it a pure transformation of configuration?** Then it belongs in
   `clash_config_compose`: immutable input, no disk, no network, no process, no
   script engine. If it needs any of those, it belongs in `clash_profiles` or
   `clash_config` instead.
3. **Does it touch the operating system?** Then it belongs in `src/platform/`,
   and core code reaches it through an interface that core owns, adapted in
   `src/app/composition/`. Never a `platform/**` include from `src/core/**`.
4. **Is it a rule about when something happens — a reload, a retry, a shutdown
   gate, a routing decision?** Then it is an application service in
   `src/app/`, with the parts it cannot own itself expressed as ports the
   composition root binds. Application services own no widgets.
5. **Is it something a person sees or clicks?** Then it belongs in `src/ui/`,
   consuming the Qt bridge and the application services. A page must not include
   an engine header; that is checked.
6. **Is it a new long-lived object, or a wiring decision between two of the
   above?** Then it is one more block in the composition root, placed so that
   what it needs is already constructed and what observes it is constructed
   after it.

Whatever the answer, the new target's dependencies go in
`tests/architecture/architecture.json` in the same change. A library that is not
declared there fails the check, and a test executable that is not declared there
is silently exempt from it — which is worse.

## Related documents

- [module-api.md](module-api.md) — the published contracts and their revisions.
- [testing.md](testing.md) — the lanes, the architecture checker, the standard
  of proof.
- [build.md](build.md) and [packaging.md](packaging.md) — how the artifacts are
  produced and staged.
- [development.md](development.md) — working conventions.
- [configuration.md](configuration.md) — profiles, presets and the enhancement
  chain.
- [migration.md](migration.md) — what changed, and what old code assumed.
