# Independent component consumer

This standalone CMake project loads the backend module, starts the explicitly
selected engine, waits for controller readiness, confirms stop, and checks that
the component image was unmapped. It includes only the public component headers
and links Qt Core and the platform loader; no desktop or backend implementation
library is linked into the consumer.

## Build

From the repository root on macOS:

```sh
make module PRESET=headless BUILD_DIR=build/p4-module
cmake -S examples/component_consumer -B build/p4-consumer \
  -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build/p4-consumer
```

The consumer is not added to the main CMake graph. To use an installed header set,
pass `-DCLASH_QT_COMPONENT_INCLUDE_DIR=/path/to/include/clash-qt` to its configure
command. This directory must contain `core/component/`.

## Run

Use a local fixture configuration with an unused loopback controller port, TUN
and DNS disabled, and no external providers or resource downloads:

```sh
CLASH_QT_DATA_DIR=/absolute/path/to/isolated-data \
  build/p4-consumer/component_consumer \
  --module "$PWD/build/p4-module/modules/libclash_qt_backend_module.dylib" \
  --engine "$PWD/build/p4-module/core/mihomo" \
  --config /absolute/path/to/fixture.yaml \
  --work-dir /absolute/path/to/isolated-work \
  --timeout-ms 30000
```

Exit 0 requires real readiness, a confirmed stop, and actual component unmapping.
The program supplies no privileged service. All artifact paths are explicit.

## ABI and lifetime

The handshake checks the module identity, target ABI, shared Qt runtime, interface
revision and wire revision. The host creates `QCoreApplication` and pumps its event
loop. Every event carries a production sequence; a host with multiple observers
uses it to reject events produced before an observer registered.

Close and drain before releasing a session. Release all returned buffers and
error objects. For unload, retain `IModuleLifetime`, release the root interface,
then call `PrepareUnload`. On refusal, restore the root through `QueryInterface`
and retry after the remaining references or work finish. Release the lifetime
interface last before closing the library handle.

The shared Qt runtime remains resident for its host cleanup callbacks. The
component itself can be unmapped. The sample checks this distinction rather than
assuming that a successful `dlclose` removed the image.

Qualification is recorded per compiler/runtime/OS. Current work is on macOS
arm64 with AppleClang 17 and Qt 6.11.1. Windows and Linux have no qualification
evidence; conditional loader code does not establish support.
