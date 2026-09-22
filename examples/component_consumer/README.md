# component_consumer

A consumer of the clash-qt backend module that is **not** clash-qt.

It exists to test one claim: that the published ABI header set under
`src/core/component/` is sufficient on its own. This program includes nothing
else from the project, links no project library, and re-implements the wire
format of `core/component/abi/wire.h` in about sixty lines — which is exactly
the job a third party would have. If a published header ever grows a dependency
on project-internal code, this file stops compiling, and that is a better
detector than a review.

It is deliberately **outside the main CMake graph** (module-r1). No build file
lives here; the coordinator owns whatever registration it eventually gets.

## Building it, by hand

macOS, against a Homebrew Qt:

```sh
c++ -std=gnu++20 -O2 -o component_consumer examples/component_consumer/main.cpp \
    -I src \
    -isystem "$(brew --prefix qt)/lib/QtCore.framework/Headers" \
    -F"$(brew --prefix qt)/lib" -framework QtCore
```

Linux:

```sh
c++ -std=gnu++20 -O2 -o component_consumer examples/component_consumer/main.cpp \
    -I src $(pkg-config --cflags --libs Qt6Core) -ldl
```

Only `Qt6::Core` and the platform loader. No `clashqt_com`, no `clash_backend`,
no marshalling library: the ABI headers are header-only by construction, and the
one function `clashqt::com::ResolveInterface` that is not is never called from
here, because a consumer implements its own `QueryInterface`.

## Running it

```sh
./component_consumer \
    --module  build/<preset>/modules/libclash_qt_backend_module.dylib \
    --engine  build/<preset>/mihomo \
    --config  /path/to/config.yaml \
    [--work-dir /path/to/dir] [--timeout-ms 30000]
```

It loads the module, performs the handshake, creates a session, installs itself
as the host, starts the engine you named, waits for readiness — an HTTP answer,
not a process start — stops it, and tears down in the order the ABI requires.

### What it will not do

* **No privileged execution.** It injects no privileged service, so every host
  command is answered `kInvalidState`. It never asks for elevation and never
  contacts an installed helper.
* **No system settings.** It touches no system proxy, VPN, network or trust
  configuration. The engine it runs is the one on the command line.
* **No discovery.** The module path is explicit. There is no search.

### What its exit code means

`0` when the core became ready, the stop was **confirmed**, and the image the
module said was unmappable really was unmapped. An unconfirmed stop is reported
and is not success — that is backend-r4 section 6, and a consumer that exits
without reading the answer has not stopped anything.

## The three things it demonstrates that are easy to get wrong

1. **Reference counting across the boundary.** `ConsoleHost` implements
   `AddRef`/`Release` by hand rather than behind a helper, because a consumer
   has to, and getting it wrong is how a module ends up calling into freed
   memory.
2. **Asking before unmapping, and then checking.** It queries
   `IModuleLifetime::PrepareUnload`, honours a `kFalse` answer by leaving the
   image mapped, and — when the answer was yes — verifies with `RTLD_NOLOAD`
   that the image actually went. "We called `dlclose`" is not the claim; a
   `dlclose` that returns 0 has not necessarily unmapped anything.
3. **Skipping the event envelope.** Every `Notify` payload begins with the u64
   production sequence of `wire.h`. A consumer with one observer has nothing to
   admit and skips it; one that re-broadcasts has to compare it against what
   each of its own observers was told had already been produced.

## The defect this program found

The first version called `dlclose` unconditionally, and after driving a real
engine to readiness and back it segfaulted there **every time**. The cause was
not what it first looked like. The faulting address was
`QtPrivate::QCallableObject<void (*)()>::impl` **in QtNetwork** — not in the
module — reached from `doActivate` inside `~QCoreApplication`, with QtNetwork
no longer in the image list.

This program links `QtCore` and nothing else, so QtNetwork was mapped **only**
because the module needed it, and `dlclose` took it away. QtNetwork registers a
host-lookup manager on `QCoreApplication::destroyed` the first time anything
resolves a name — which is why only the stage that started the engine crashed,
and why loading, creating a session, installing a host and setting a binary path
all unmapped cleanly.

The fix belongs to the module, not to this program: it pins the shared runtime
it brings into the process before it constructs anything, and then its own image
is genuinely unmappable. This program now checks that it was.
