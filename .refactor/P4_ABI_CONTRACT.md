# P4 module boundary — module-r1

Coordinator architecture decision, 2026-09-22; applies D8 without changing
MihomoBackend or BackendBridge. Existing component-r1 vtables stay immutable.

- A separately built SHARED library wraps MihomoBackendImpl; never proxy traffic.
- Exported C factory performs ABI version + target ABI handshake before activation,
  outputs null on failure; fixed-width fields, UTF-8 pointer+length inputs, owned
  IBuffer results, noexcept COM interfaces, fresh owned IIDs. No Qt/STL values
  cross the binary boundary. Host/module use matching shared Qt and a host
  QCoreApplication; sample may link Qt Core but no desktop/backend implementation.
- New ABI headers under core/component/abi/ are implemented under an explicit
  lease against this contract and reviewed by coordinator before publication.
  Fixed vtables may use a versioned command/event protocol; all operations/events
  of backend-r4 must round-trip losslessly including 64-bit IDs/counters.
- Telemetry connections use one packed buffer, fixed-layout record offsets into
  UTF-8 strings; validate all sizes/offsets and report malformed packets.
- Privileged execution stays host-owned. Marshal the existing injected service
  through an ABI-safe reverse interface; one connection only, no default real
  helper construction in module. Host and callbacks outlive pending work.
- Loader in integrations/component selects explicit/installation-relative artifact,
  validates module identity and target/version. No source/build/PATH fallback.
  Module lifetime must cover objects, buffers, callbacks and pending operations;
  close/cancel/drain before unloading, unconfirmed stop cannot be success.
- Fake module uses the same adapter/marshalling around FakeBackend, built for tests
  only. Common assertions run against in-process fake, in-process real, module
  real and module fake through fixture adapters. Existing specialist suites stay.
  Do not add failure-inversion switches to shipping ABI.
- Shared module may compile existing implementation sources directly to avoid
  turning the sealed private static target into a new permanent exception.
  App and five journeys switch to loader/shim, clearing both migration exceptions.
- Independent sample project builds outside main CMake graph using public ABI
  headers and platform loader/Qt Core only, and drives pinned local engine.
- Qualification claims limited to measured macOS arm64 compiler/runtime/Qt;
  Windows/Linux (and untested architectures) remain explicitly unqualified.

Registrations and packaging are coordinator-only. Workers return exact source
list, target dependencies and how each acceptance claim was tested/inverted.
