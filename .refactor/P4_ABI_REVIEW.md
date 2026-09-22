# Coordinator review — ABI gate remains open

Initial ABI worker's real-core consumer reached ready and confirmed stop, then
segfaulted on dlclose. Skipping dlclose avoided the crash. That establishes an
unload defect, not its cause: a Qt-registry explanation needs a stack/reproducer.

The worker's interim PinnedOnceActivated/kFalse path is **not accepted as gate
closure**. An inactive/fake module unloading does not prove that the activated
shipping module unloads. Restore actual safe teardown and demonstrate it; do not
rewrite the gate around a permanent mapping. Any unavailable evidence stays open.

Other integration requirements identified by the coordinator:

1. Every module-owned buffer/error object (including GetMessage/GetSource buffers)
   must retain the module and participate in lifetime accounting after session
   release. Session-only counts are insufficient.
2. Module root is free-threaded. Serialize CreateObject versus PrepareUnload;
   independent check/unload atomics leave a check-to-act race. Retained root
   interfaces also matter. No exceptions escape/terminate allocation failures at
   COM entry points: return failure with null output.
3. Validate response struct size before every handshake write, including null
   request/output error paths.
4. Observer admission uses event **production**, not host Notify receipt. A newly
   added observer must not receive already-produced queued events. A proposed
   concrete adaptation: expose produced/current-delivery sequence internally in
   the real/fake backend dispatchers; publish it as fixed-width event metadata
   and a query command through the module; host registration records produced
   sequence. All public MihomoBackend/Qt bridge signatures remain unchanged.
   If a new ABI vtable signature is needed, allocate a fresh IID; do not mutate
   the existing one. Synthetic test telemetry must follow the same sequencing.
5. Target ABI headers should compile with MSVC/clang-cl as well as Clang, without
   implying Windows evidence. Qualification belongs to measured toolchain/OS/
   runtime records, not an unconditional claim for all macOS arm64 compilers.

Expanded repair lease may include the private real/fake dispatcher and CoreProcess
sources if evidence shows they own the retained callback. No host UI/bridge rewrite,
new helper connection or change of process/data-plane architecture is authorized.

Additional qualification case for the final read-only audit: component-r1 permits
AddRef/Release from any thread. Releasing the final session reference from another
thread must not destroy QObject/QProcess collaborators on the wrong thread; if
cleanup is deferred to the owner thread, the library remains counted/pinned until
that cleanup completes. Test this against an activated session, not just an inert
COM object. A method-affinity note cannot silently narrow IObject's refcount rule.

## Coordinator decision after the debugger evidence

`abi-fix/repro/images.log` proves dlclose returns 0 and the component image is
absent. The later PC is 0x101fd1c44 inside the formerly mapped QtNetwork image
(base 0x101fb8000); only QtCore remains mapped. Thus the initial explanation of
component meta-object retention was not established and was too broad.

It is acceptable to retain the **shared Qt runtime dependencies** for the host's
runtime lifetime: they own cleanup callbacks registered with the still-live Qt
runtime. This is distinct from pinning the component itself and must be documented
as such. Prove component absent / runtime present, then clean host shutdown after
an activated real-engine session. No retained callback may point into the unmapped
component. The component's own root/session/buffer/error objects still require
complete reference and queued-work accounting; runtime retention cannot hide them.

Marshalling audit: QDateTime equality compares instants, so it alone cannot prove
lossless representation. Current codecs store only epoch milliseconds and decode
in local time. Check UTC/fixed-offset/named-zone values as well as invalid values;
Connection details render start/end with toString(Qt::ISODate), making a changed
zone/offset observable even if operator== succeeds. Preserve the host-visible
representation or document a coordinator-approved semantic choice; no implicit UI
change is intended by D8.

Reference-count clarification for final recheck: component-r1 says a returned zero
from Release means the object has been destroyed. If the COM session object itself
is retained for owner-thread cleanup, do not return zero early and silently redefine
that promise. Hold/transfer a cleanup reference and report a nonzero count until
actual destruction, or destroy the Qt-free COM wrapper and separately count native
cleanup state. This does not require synchronous stopping from Release; explicit
Close/drain remains the way to obtain an operation outcome.
