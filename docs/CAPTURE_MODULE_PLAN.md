# Application monitoring and MITM modules

Status: proposed, 2026-09-21. Two requested add-ons sharing target selection, session
storage and scheduling. This plan changes no routes, certificates, applications or
containers. Implements goals G7/G8 in [PROJECT_GOALS.md](PROJECT_GOALS.md).

## Product contracts and visibility

**Monitor:** select a host app/program or container workload and observe inbound/
outbound connections, endpoints, destinations, timing and traffic volume. Run its
analysis/index/API backend in an owned container. No TLS decryption or body retention
is required in monitor mode. Parse only enough plaintext metadata to report URLs
where actually observable; record provenance and visibility limits for every field.

**MITM:** select an app/program, capture supported requests and responses, decode
supported content for inspection, and forward through a separately established
upstream connection. Save sessions, inspect content, export captures and run bounded
periodic capture jobs with optional user-selected scripts. Use the local
`3rdparty/mitmproxy` source and its mitmdump worker; preserve its upstream role rather
than rebuilding its TLS/protocol stack in C++.

| Observation | Monitor | MITM |
| --- | --- | --- |
| Local/remote IP, port, transport, timing and direction | Where the selected sensor can observe/attribute it | Captured flows plus targeting evidence |
| Bytes sent/received | At the declared observation layer; distinguish payload/wire counters | Captured message/connection counts with defined units |
| DNS name / TLS server name | Only observed plaintext DNS/SNI or explicitly sourced app metadata; encrypted DNS/ECH may hide it | Available for supported intercepted traffic; do not infer unobserved DNS events |
| Full HTTP URL | Plaintext HTTP headers where observed; not guaranteed for encrypted traffic | Scheme/authority/path/query for supported decoded HTTP; URL fragments are not sent over HTTP |
| HTTPS path/query and request/response bodies | Unavailable without decryption | Requires successful interception and the target's trust configuration |
| App-encrypted or unknown protocol bodies | Opaque/unavailable | Raw bytes where captured; semantic decode needs an explicit supported decoder and keys where applicable |

Do not present reverse-DNS guesses as observed URLs. HTTP CONNECT exposes authority,
not the encrypted request path. Preserve observed and inferred fields separately.
Distinguish response bytes on an outbound connection from a new incoming connection
accepted by a server app; the latter needs its own capture capability.

"Decode all content and re-encode as before" means full request/response content for
qualified app/protocol combinations, preserving application semantics in capture-only
mode. It cannot promise all applications/protocols or identical packets: interception
creates new TLS sessions and may change HTTP framing, compression or protocol version.
Certificate pinning, custom trust stores, mTLS, application-layer encryption and
unsupported protocols are explicit compatibility states. Do not silently disable
upstream certificate verification or claim an opaque flow was decoded.

## Source-backed implementation constraints

The local source reviewed is mitmproxy submodule commit
`b506c68108e287104045333ade476d92c39c275e`; actual build provenance is recorded when
implemented. The following local references supply the current capability baseline:

| Reference | Implication |
| --- | --- |
| [Proxy modes](../3rdparty/mitmproxy/docs/src/content/concepts/modes.md) | Local mode supports process name/PID selection on macOS/Windows/Linux; Linux/macOS are egress-only, with replies included. Reverse mode is needed for qualified inbound-server interception |
| [Protocol support](../3rdparty/mitmproxy/docs/src/content/concepts/protocols.md) | HTTP/1, HTTP/2, WebSocket and other modes exist; HTTP/3 is mode-limited, raw TCP/UDP is not universal semantic decoding, STARTTLS is not detected |
| [Certificates](../3rdparty/mitmproxy/docs/src/content/concepts/certificates.md) | Target trust is required; pinning may reject interception |
| [Mode servers](../3rdparty/mitmproxy/mitmproxy/proxy/mode_servers.py) | Local redirector lifecycle must be coordinated; stopping interception does not necessarily terminate the helper |
| [Native flow save](../3rdparty/mitmproxy/mitmproxy/addons/save.py) | Native flow streaming/filtering/rotation is available; disk failures need supervisor handling |
| [HAR save](../3rdparty/mitmproxy/mitmproxy/addons/savehar.py) | HAR generation retains flows in memory; use bounded/offline export, not unlimited live HAR capture |
| [Proxy server](../3rdparty/mitmproxy/mitmproxy/addons/proxyserver.py) | Streamed bodies need an explicit storage policy; enabling storage has resource costs |
| [Add-on API](../3rdparty/mitmproxy/docs/src/content/addons/overview.md) | Event scripts exist; periodic app capture scheduling is clash-qt functionality to implement |
| [Runtime dependencies](../3rdparty/mitmproxy/pyproject.toml) | Python >=3.12 and native mitmproxy_rs dependencies must be pinned and packaged/tested per OS |

Linux local capture has documented kernel/privilege constraints, truncated process
name matching, host-network container restrictions and no default WSL support.
Validate the real kernel/helper capabilities; do not infer compatibility from a
distro name alone. The source's local targeting is not proof of a PID on every
stored flow: build and verify an attribution adapter, with unknown/ambiguous states.

## Module architecture

```text
Capture UI -> CaptureCoordinator -> portable COM capture interfaces
  ├─ MonitorBackend -> host sensor -> authenticated metadata channel
  │                                -> container analysis/index service
  └─ MitmBackend -> bounded local IPC -> source-built mitmdump/Python worker

Shared: TargetResolver, CaptureStore, ExportService, ScheduleService
Host-owned: network plans, native capture permissions, trust changes, cleanup
```

C++ adapters implement the portable COM foundation derived from the reference in
[COM_MODULE_PLAN.md](COM_MODULE_PLAN.md), with project-owned names and IDs. Go,
Python and container processes communicate via versioned messages; object pointers
and reference counts do not cross process boundaries.

Initial contracts: `ICaptureCapabilities`, `ICaptureTargets`, `ICaptureSession`,
`ICaptureCatalog`, `ICaptureExport`, and `ICaptureSchedule`. Publish immutable values
for target identity, session options, ready/applied state, flow metadata, artifacts,
quota/completeness flags, and terminal outcomes. Keep large bodies out of synchronous
ABI calls: use bounded streams/artifact handles and explicit ownership.

Target identity includes executable path, available bundle/signing identity, PID
and process start time. Jobs match stable executable identity and resolve the current
process instance at execution time. A reused PID is not the original app. Specify
whether child processes are included; do not assume a program-name match is unique.
Sessions report selected targets separately from verified per-flow attribution.
Record readiness/start time as the capture boundary. Earlier requests/body bytes
cannot be recovered. Qualify already-open sockets separately from connections made
after readiness, and distinguish pre-start missing data from dropped live events.

## Containerized monitor deployment

Host-app attribution needs a host sensor, even when analysis runs in a container.
Docker Desktop introduces a VM/network boundary; a container does not automatically
observe host processes or their sockets. See [Docker Desktop networking](https://docs.docker.com/desktop/features/networking/).

Implement capability-qualified adapters: Linux can evaluate socket/eBPF/cgroup
events with explicit kernel/privilege requirements; macOS and Windows need native
process/network APIs and possibly signed/privileged helpers. These are engineering
spikes, not already-proven full coverage. Socket polling can be a limited fallback
but must report missed short-lived connections and unavailable byte counts.

For container targets, map runtime/container identity and network namespace to the
sensor. Do not assume Linux host-network examples work for ordinary bridged workloads
or Docker Desktop host apps. Sensor emits metadata after app attribution and payload
discard; plaintext URL-header parsing is bounded and optional. Raw packet/body
archives are outside monitor mode's default contract.

Container receives normalized events, stores/indexes them, and serves loopback-bound
queries. Use an authenticated explicit channel and owned volume. Avoid blanket
privileged containers, host filesystem mounts or a Docker socket inside the analytics
container. The host manages only its labeled containers. Pin image digest/source
provenance. Backpressure reports dropped-event counts, gaps and collector coverage.

Every platform matrix row records host versus container target, initiated versus
accepted connections, IPv4/IPv6, TCP/UDP, process attribution, timing and counter
support. Unknown fields are allowed; invented completeness is not. Both directions
of selected applications are the feature goal, with gaps exposed until qualified.

## MITM transport, trust and routing

First support an explicitly launched/selected proxy-aware app and a regular proxy
listener. Then qualify native local process capture per OS. Native mitmdump is the
default for host-app capture; container deployment may host an explicit proxy for
apps that can reach it, but is not a universal replacement for local interception.

Use one host capture broker to coordinate local redirectors and overlapping sessions.
Do not launch competing redirectors for every schedule. If concurrent app attribution
cannot be proven, serialize sessions or report the unsupported combination. Include
helper status in cleanup even when the upstream helper remains alive by design.

NetworkCoordinator owns the effective route, such as selected app → MITM → mihomo
→ destination, with sensor/worker/core/controller/collector exclusions to prevent
loops. Prototype mode/upstream combinations before promising arbitrary chaining:
regular-upstream proxy, local interception and TUN each require distinct validation.
Reuse the network-plan conflict rules for Tailscale/EasyConnect and other apps.

Readiness checks cover target selection, listener/redirector availability, trust
for that target and storage readiness. CA keys remain in protected local session/
workspace storage; prefer target-specific trust where the app supports it. Trust-store
changes are an explicit user action, tracked separately from starting capture and
reverted only when owned and requested. Never install a CA as a build or automatic
schedule side effect. Keep upstream TLS validation enabled. Pinned/mTLS apps require
an explicit supported test configuration, otherwise show uninspectable/failure state.

Responses on captured outbound connections are part of the same session. To inspect
new inbound connections to a selected server, add an explicit reverse listener with
known destination and certificate/routing settings; do not equate it with local
egress mode. Validate Windows inbound capabilities separately rather than inferring
them from the other OSes.

## Capture data, saving and export

Store a versioned manifest plus native mitmproxy flow artifacts, bounded indexes
and optional payload blobs. Include target attribution/provenance, engine versions,
mode, capture filters, timestamps, byte-count layer, script hashes, drops/truncation,
and session completion/error state. Passive metadata uses the same catalog but never
pretends to contain bodies. Keep captures outside ordinary app settings backups.

Native flow files are the MITM source artifact. Export filtered native flows, JSON/
CSV metadata and bounded HTTP HAR; offer binary/text payload export where available.
PCAP, if later provided, has separate packet semantics and cannot replace decoded
flows. HAR cannot faithfully represent every supported protocol or infinite stream.
Redacted exports are derived artifacts; do not silently overwrite originals.

Capture-only is the default: record both directions without mutation scripts.
Preserve original captured bytes when available and decode into derived views.
Qualified body capture covers compressed, binary, multipart, streamed and WebSocket
content with explicit limits. Full-body mode requires capacity checks; if quota or
disk failure prevents completeness, mark the session incomplete or stop with a clear
outcome. Never silently discard streamed bodies while displaying 'full capture'.

Use bounded memory/IPC queues, file rotation, atomic manifest updates and crash
recovery that marks partial artifacts. Retention, per-job quotas, output paths and
export redaction are user-configurable. Captures can contain credentials/content:
restrict local access and keep sharing/export explicit. Encryption at rest can be
added with a credential-store-backed key; do not describe plaintext files as encrypted.

## Periodic capture and scripts

Persist jobs in clash-qt, not in Codex automations or free-form OS cron commands.
Each job declares target identity, monitor/MITM mode, schedule/timezone, bounded
duration, protocol/host filters, body policy, storage/retention, script references
and overlap/missed-run behavior. Distinguish capture filters from routing selection:
filtering saved flows alone must not intercept unrelated apps.

Default to skipping overlapping runs, no automatic catch-up after sleep, and no
launching a stopped app unless an explicit launch action is configured. Re-resolve
target identity and readiness on each run. Distinguish skipped (app unavailable),
blocked (trust/permission missing), failed and complete/incomplete outcomes.
Define interval monotonic-time behavior and wall-clock/DST scheduling; persist run
IDs to prevent duplicate work after restart. Stop/cancel flushes artifacts and restores
owned changes. Initial scheduling runs while the app is active; always-on jobs need
an explicitly enabled user service using the same contracts and shutdown behavior.
Give active HTTP/WebSocket/TCP flows a bounded stop/drain grace period. After the
deadline, terminate according to the session policy, mark remaining artifacts/flows
partial and restore owned routing. A bounded job must not wait forever for a stream.

Support versioned local mitmproxy add-on scripts selected by the user and optional
pre/post-capture actions with structured arguments. Scripts execute in the worker
under the user's privileges, not as privileged helper commands. Process isolation
is not a script sandbox. Track script version/hash, errors, timeout and termination
behavior. A script crash/hang cannot silently leave an active redirector. Changing
content requires an explicit transform script; ordinary scheduled capture preserves
traffic semantics. Remote managed presets do not automatically execute arbitrary code.
Mitmproxy can reload scripts when files change. Snapshot each selected script and
its declared resource files into an immutable run workspace and record hashes; jobs
do not silently adopt edits halfway through a run. Explicit live-reload sessions, if
added later, must record every activated version and its effective time.

## Implementation slices and tests

| Slice | Deliverable | Acceptance |
| --- | --- | --- |
| CAP-0 | Target/session/event/storage/IPC contracts and platform capability probes | Monitor/MITM separation, attribution evidence and constraints recorded; fake contracts tested |
| MON-1 | Host sensor plus container event/index backend, metadata UI | Selected app and server's observed in/out metadata; unrelated app excluded; no body retention; unknowns/gaps honest |
| MITM-1 | Local source-built worker, regular app proxy and native flow save | Local HTTP/HTTPS request and reply preserved/decoded; untrusted/pinned/invalid upstream cases accurate |
| CAP-DATA | Catalog, retention, bounded exports and periodic jobs | Round trip, partial files, disk failure, cancellation, restart, DST and duplicate prevention |
| CAP-UI | App selector, live monitor/inspector, content/export and schedule UI | Clear capability/trust/readiness/coverage state and ordinary workflows |
| CAP-NATIVE | Process-targeted native interception, inbound reverse case and coexistence | Each OS/mode tested; PID reuse/children addressed; own workers excluded; cleanup verified |
| CAP-RELEASE | Optional add-on packages and source/runtime/image provenance | Fresh install of monitor/MITM add-ons; missing dependencies do not break proxy app |

Tests include two synthetic client apps (selected/unselected), a synthetic server,
ordinary inbound/outbound traffic, local DNS, HTTP/1/2, qualified HTTP/3, compressed/
binary bodies, WebSockets and long streams. Negative tests cover no-trust/pinning,
opaque app encryption, unavailable helpers, container restarts, process restart/PID
reuse, IPC backpressure, native-save errors, quota limits and script failures. Assert
application content and attribution, not identical encrypted packets. Native tests
use disposable trust/network environments; routine CI uses local fixtures and mocks.
Also cover pre-existing versus newly opened connections, editing the source script
during a run, and stop/cancel while a long-lived stream is active.

## Files and parallel work

Shared contracts: `src/core/capture/` and component interfaces in `src/core/component/`.
Coordination/targets: `src/app/capture/`; sensors: `src/platform/capture/<os>/`;
trust adapters: `src/platform/trust/<os>/`. Backend modules:
`src/integrations/capture/monitor/` and `src/integrations/capture/mitmproxy/`.
Worker/add-on bridge: `src/services/capture/mitmproxy/`; container service:
`services/monitor/`; image/package files: `packaging/capture/`. Catalog/exports:
`src/core/capture/store/`; schedules: `src/app/capture/schedules/`; UI:
`src/ui/pages/capture/`. Tests mirror contracts/providers/app/UI plus synthetic
fixtures and native cases; root coordinator owns shared contracts/build/shell wiring.

After CAP-0, backend providers and storage/scheduling can progress in three slots.
UI and native/package validation run in subsequent slots after data/provider
contracts are handed off. Schedule these as P8/P9 in the parallel plan, or pull a
ready package into a freed earlier slot once component/network/target contracts are
integrated. They do not wait for an external plugin SDK; implement built-in modules
first. They are requested product scope, not prerequisites for the earlier foundation
refactor/main cutover. No placeholder tests count as delivered features.
