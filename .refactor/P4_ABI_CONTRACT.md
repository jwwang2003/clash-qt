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

## Qualified implementation details (P4 integration)

- Module ABI remains 1; the current wire revision is 3 and connection snapshot
  format is 2. Wire 2 added production-sequence admission; wire 3 preserves
  timestamp validity, time specification, offset and zone identity. These payload
  changes did not change component-r1 vtables or the Qt-facing facade.
- The host creates QCoreApplication. Shared Qt runtime dependencies remain mapped
  for their process/runtime cleanup callbacks; this does not pin the component
  image. Qualification checks actual component unmapping and clean host exit.
- For unloading, hold IModuleLifetime, release the root interface, then prepare.
  Preparation requires zero children/work and no other root interfaces. On refusal
  the loader restores its root and remains usable; release the foreign references
  and retry. Release the lifetime interface last before closing the library handle.
- A final Release that must defer destruction retains a cleanup reference and
  returns nonzero. Returned zero retains component-r1's destruction meaning.
  Owner-thread native cleanup and module-defined runnables remain counted until
  finished; Close(timeout) cannot report quiescence while work survives.
- Handshake failures validate response size before every write. Session decode
  failures publish their own diagnostic rather than exposing a stale one.
- Compatibility claims remain limited to measured macOS arm64 artifacts. Code
  paths for other platforms do not constitute compile or runtime evidence.
