# macOS privileged helper protocol

The helper is an independent Objective-C++ executable in [macos_helper.mm](../src/service/macos_helper.mm). It uses Foundation and POSIX interfaces and links only Apple system frameworks/libraries; it does not link Qt, Homebrew libraries or the application's UI runtime. It is a macOS implementation, not a Windows/Linux service implementation.

## Installation and ownership

The GUI obtains explicit administrator authorization before invoking:

```text
clash-qt-service-helper --install OWNER_UID /absolute/path/to/approved/mihomo
clash-qt-service-helper --uninstall
```

`OWNER_UID` must name an existing non-root local account with UID at least 501. Installation/removal require effective UID 0. The installer reports one machine-readable completion line to stdout:

```text
CLASH_QT_SERVICE_RESULT {"ok":true,"error":""}
```

Errors use `ok:false`, an explanatory string, and a nonzero exit status. All subprocess invocations use fixed executable paths and argument vectors, without shell command interpolation.

Installed locations:

| Object | Path | Protection |
|---|---|---|
| Helper | `/Library/PrivilegedHelperTools/org.clash-qt.service/helper` | root-owned executable, 0755 |
| Approved core | `/Library/PrivilegedHelperTools/org.clash-qt.service/mihomo` | root-owned executable, 0755 |
| LaunchDaemon | `/Library/LaunchDaemons/org.clash-qt.service.plist` | root-owned, 0644 |
| Runtime | `/Library/Application Support/org.clash-qt.service` | root-owned directory, 0700 |
| Socket parent | `/var/run/org.clash-qt.service` | root-owned directory, 0755 |
| Socket | `/var/run/org.clash-qt.service/socket` | selected owner's UID, 0600 |

The daemon runs the fixed installed helper as root with `--serve OWNER_UID`. The installer validates the approved helper/core as regular Mach-O executable files owned by root or the explicitly authorized user, without group/world write or set-ID bits. Sources are opened with `O_NOFOLLOW`; copying reads the opened descriptor, checks size/modification metadata, and installs through a private temporary file plus rename. Root-owned destination directories cannot be symbolic links or group/world writable. The installer stops the old launchd instance before replacing its plist and bootstrapping the new instance. Removal stops launchd and removes only the fixed service files and private service runtime directory.

The GUI must stop its active core lease before install/remove. Updating the selected executable in ordinary app settings does not replace the privileged copy: reinstalling the helper with administrator authorization selects a new privileged binary.

The current GUI installer uses the deprecated macOS `AuthorizationExecuteWithPrivileges` interface and presents administrator authorization only for an explicit install/remove action. This is an interim development integration; signed distribution and a modern ServiceManagement installation lifecycle require separate work. It is not an App Store/sandbox deployment claim.

## Wire framing

Each frame is a four-byte unsigned **big-endian** payload length followed by UTF-8 JSON. The maximum payload is **8 MiB**; zero-length and oversized frames close that connection. An incomplete frame must arrive within ten seconds. The helper permits at most eight concurrent connections, verifies each socket's peer UID using `getpeereid`, and accepts only the configured owner UID.

Every request contains `protocol:1`, an integer `id` in `0..2^53-1`, and `command`. Unsupported protocol versions return an error; clients must also validate the response protocol. No command accepts an executable path, shell command or configuration file path.

```json
{"protocol":1,"id":1,"command":"status"}
{"protocol":1,"id":2,"command":"logs"}
{"protocol":1,"id":3,"command":"start","config":{"external-controller":"127.0.0.1:29097","secret":"0123456789abcdef0123456789abcdef","tun":{"enable":true}}}
{"protocol":1,"id":4,"command":"stop"}
```

Responses echo the request ID:

```json
{
  "protocol":1,
  "id":3,
  "ok":true,
  "error":"",
  "state":"running",
  "pid":123,
  "endpoint":{"host":"127.0.0.1","port":29097,"secret":"0123456789abcdef0123456789abcdef"}
}
```

`state` is `running` while the child process exists and `stopped` otherwise. A successful `start` means the process was spawned; the GUI must separately check authenticated controller readiness. Startup failures can appear as a subsequent stopped state and core log output. `endpoint` is present once a launch has an assigned endpoint; stop clears it. `logs` adds an array containing at most the last 200 lines from a bounded 64 KiB stdout/stderr buffer. Log draining is bounded per event-loop iteration so output cannot indefinitely starve command handling.

## Core lease

The connection that successfully issues `start` owns the core lease. Another connection can read `status`/`logs` but cannot replace or stop that leased core. Disconnecting an observer does not stop the core. Disconnecting the owner stops the child, as does helper SIGTERM/SIGINT. Stop sends SIGTERM, drains output while waiting up to three seconds, then sends SIGKILL and reaps the child if necessary. A successful stop response follows actual child exit.

The client must retain its connection for the entire privileged session. Application termination/crash closes the connection; a persistent daemon without a GUI lease does not intentionally leave a core running. This is a socket-lifetime lease, not a periodic heartbeat protocol.

## Configuration boundary

`start.config` is a JSON object converted from the managed runtime configuration. The helper creates a new root-private session directory and writes a 0600 JSON configuration there; mihomo accepts JSON as YAML. The child always executes the fixed installed core with the persistent root-private runtime directory as its home and the session's absolute configuration path, a minimal environment, and no inherited client descriptors. Geodata and mihomo's selected-node cache persist across restarts inside the private runtime directory; session configuration/provider cache files are removed after stop.

Core-file serialization deliberately disables Foundation's optional JSON slash escaping. Mihomo's YAML parser rejects `\/` with `yaml: found unknown escape character`, even though that escape is valid JSON. Wire-protocol JSON is unaffected. The configuration self-test checks literal slashes, and the test-only `--core-config-self-test /absolute/path/to/mihomo` passes the helper's actual sanitized/serialized synthetic output to an unprivileged `mihomo -t` process. This regression was reproduced with escaped slashes and verified fixed against mihomo v1.19.29.

The helper enforces:

- Controller binding to `127.0.0.1`, port 1024–65535, and a 32–256 character alphanumeric/`-`/`_` secret. The app-generated secret/port are preserved so readiness checks agree with the launch response.
- Removal of alternate controller transports, external UI/download settings, top-level TLS file settings, custom geodata URL/loader settings, and externally supplied profile storage settings. Core defaults supply geodata; automatic geodata updates are disabled by the helper.
- A fixed profile-storage policy inside the private work directory. CORS is reset to an empty allowed-origin list.
- HTTP(S) and inline providers only. Local `file` providers are rejected with an actionable error. Provider cache paths are replaced with sequential filenames in the private work directory; names and original path strings cannot select a filesystem location.
- Rejection of other explicit file/path/private-key/certificate/socket fields and `file:`/`unix:`/`unixgram:` URLs. WebSocket, HTTP and HTTP/2 transport `path` fields are explicitly allowed as URL paths.
- A nesting limit of 64 and the frame/configuration size limit. Unsupported file-backed settings must be converted to supported inline configuration or used with an unprivileged core.

**This is a trusted-owner privileged-core model, not a sandbox for hostile configurations.** The explicitly authorized owner chooses the core binary and subscription/configuration contents. HTTP(S) providers are subsequently downloaded and interpreted by mihomo. The authenticated controller secret grants control over that privileged core, including its own management APIs; initial helper configuration checks do not sandbox later controller operations or downloaded provider content. Do not distribute that secret or represent this service as permitting untrusted users/configurations safely. Other local UIDs cannot connect to the helper socket; the controller remains loopback and authenticated.

## Verification and remaining limits

Unprivileged validation command:

```sh
clash-qt-service-helper --self-test
```

The helper refuses self-test as root. The self-test exercises endpoint/secret constraints, TUN preservation, file-provider and unsafe local credential/URL rejection, provider path confinement, stripped controller/UI fields and ordinary transport URL paths. It does not install anything, open the service socket, start mihomo, change proxy/TUN state or contact a network endpoint.

The separate test target compiles the same source with `CLASH_QT_HELPER_TESTING=1` and runs `--ipc-self-test`. Only that test binary contains alternate-path injection and a fake-core mode. The production binary exposes neither a test server endpoint nor path overrides. The test refuses privilege, forks the actual server event loop in UID-owned temporary directories, authenticates a real Unix socket peer, and launches an unprivileged fake core. It verifies framing/version/ID responses, owner start/stop, read-only observers, rejection of observer mutation, observer disconnect isolation, owner disconnect termination and reaping, oversized-frame disconnect, partial-frame timeout, and socket cleanup. The partial-frame deadline is shortened only in this test build.

The IPC test exposed and fixed a fork/exec signal race: a child could briefly inherit the helper's SIGTERM handler and consume a disconnect-triggered termination before exec. SIGTERM/SIGINT are now blocked around fork, reset to default in the child, then unblocked. The child performs only POSIX operations between fork and exec.

During initial implementation the helper was compiled separately with Apple Clang, its nine configuration self-tests and real-server unprivileged IPC harness passed, and `otool -L` showed only system dependencies. A subsequent manual user installation exposed the slash-escaping regression described above; its fix was validated with the real core without privilege. **Those automated tests do not establish administrator installation, launchd integration, privileged TUN routing, helper removal, or clean-machine signed distribution.** The harness validates the actual server/protocol/lease behavior as an ordinary user; it does not establish root policy, cross-UID rejection under privileged installation, or real mihomo routing behavior. Rebuilding the app does not replace an already-installed privileged helper; installation must be repeated through its explicit administrator-authorized UI action to deploy a helper fix.

Subsequent live acceptance on the development Mac verified user-authorized install/repair, launchd operation, UID-0 mihomo startup, controller readiness, TUN `utun9` enable/readback, HTTP 204 forwarding without configured proxies, restart persistence, and stop-time route restoration to `en0`. The installed helper hash matched the tested bundled binary. This does not extend that validation to removal, crash/power-loss recovery, other machines or signed release distribution.
