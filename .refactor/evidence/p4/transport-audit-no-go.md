# Targeted transport parity audit — findings (read-only lease)

Subject: `git archive HEAD` (f04647f) unpacked at `snap/`, built standalone at
`build/` (`-DCLASH_QT_BUILD_APP=OFF`, homebrew yaml-cpp). Repository untouched.
`snap/tests/CMakeLists.txt` was trimmed to the fixtures the audit needs
(original kept as `CMakeLists.txt.head`) because HEAD's
`tests/core/mihomo/CMakeLists.txt` references module targets whose registrations
are still in the coordinator's working tree, so HEAD alone does not configure.

Suite: `snap/tests/transport_audit/transport_parity_audit_test.cpp`, target
`transport-audit-tests`, 9 cases. Every case asserts the CONTRACT-required
behaviour, so a FAIL is the defect. Observation is through `BackendObserver`
callbacks and the fixture request/handshake log only.

Every run: `run_check.py` (isolated `CLASH_QT_DATA_DIR`, `QT_QPA_PLATFORM=offscreen`,
`QT_QUICK_BACKEND=software`), preferences hashed before and after.
`com.clash-qt.clash-qt.plist` = `6817843…78371c` before and after every run
(see `logs/*.txt` and `/tmp/clash-qt-p4.w0Y8Uo/check-runs.jsonl`).

## Verdict: NO-GO on the identified defect class. 8 of 9 cases fail.

Baseline: `logs/baseline-final.txt` (3 passed, 8 failed).

### F1 — providers never learn of a same-address replacement (root cause)

`provider_client.cpp:37-42` subscribes to `MihomoClient::endpointChanged` only.
`mihomo_client.cpp:44-74` (same address, new session) bumps `requestEpoch_`,
emits `invalidating()`, and deliberately does NOT emit `endpointChanged`. So
`ProviderClient::epoch_` never moves, and neither does anything derived from it:

* the coalescing key `QString::number(epoch) + ':' + path` (`:71`, `:139`);
* the reply guard `epoch != epoch_ || !sameEndpoint(endpoint)` (`:82`, `:148`) —
  `sameEndpoint()` compares `httpBase()+secret`, which a replacement preserves;
* the abort loop in `setEndpoint` (`mihomo_client.cpp:65-66`) iterates
  `MihomoClient`'s own `network_`; `ProviderClient` owns a second
  `QNetworkAccessManager` (`provider_client.cpp:36`) that nothing aborts.

Observed, three ways:

| case | observed |
| --- | --- |
| `aProviderFetchAfterAReplacementIssuesUnderTheNewGeneration` | post-replacement `fetchProviders()` returns the RETIRED request's id and issues nothing; `requestCount("GET","/providers/proxies")` stays 1 |
| `aRetiredProviderReplyIsMarkedSupersededAndCarriesNoPayload` | retired reply settles `CompletionStatus::Ok` with the retired payload — backend-r2 A2 requires the MARK |
| `theAnswerToAPostReplacementFetchIsNotOneTheConsumerMustReject` | the completion answering a fetch submitted NOW carries a generation older than what the consumer had already observed, so §2's mandatory rejection rule discards the consumer's own live request |

Contrast with the REST path, which is correct and already covered:
`backend_real_contract_test.cpp:452` (`aReplacementAtTheSameAddressCannotClearTheNewSession`)
proves a held `/version` across the same replacement settles `Superseded`.

### F2 — a generation bump that is not a client event marks nothing

`MihomoBackendImpl::start()` (`mihomo_backend.cpp:794-805`) and the managed
failure handler (`:371-383`) bump the global generation without invalidating
anything on `MihomoClient`. `settleRest` (`:621-647`) and the provider settle
(`:318-343`) derive `superseded` solely from the collaborator's flag, and
`publish()` (`:649-657`) forwards that status unchanged — it never compares
`request.generation` against `generation_`.

Measured (`logs/baseline-final.txt`, QINFO line):
`version completion: status=0 generation=1 observedBefore=3 payload=retired-session`.
The consumer HAD observed generation 3, so §2's second line of defence does
fire here; what is missing is A2's primary one — the completion is `Ok` and
carries the retired payload, so a consumer that follows A2 as written accepts
stale data. Same for a provider reply held across the managed failure
(`aProviderReplyHeldAcrossAManagedFailureIsMarkedSuperseded`).

### F3 — retired-session streams are the live subscription

`openStream` gates every callback on `endpointEpoch_` (`mihomo_client.cpp:748-769`),
which a same-address replacement does not move, and that path deliberately
leaves the sockets open (`:70-71`).

* `aRetiredStreamFrameIsNotPublishedAsTheNewSessionsTelemetry`: a frame written
  by the retired session after the replacement is published as a `trafficSample`
  stamped with the REPLACEMENT's generation. No consumer rule can reject it —
  unlike F1/F2, neither line of defence exists.
* `aRetiredStreamDropDoesNotClearTheNewSession`, measured:
  `connectionTransitions 1->3, liveStateClears 1->2, connectionListClears 1->3`.
  The retired socket's death runs `setConnected(false)` → `invalidating` →
  `++requestEpoch_` → abort → `clearLiveState()`, i.e. it reports the healthy
  replacement as disconnected and clears the view the replacement had just
  populated. That is exactly what `backend_real_contract_test.cpp:452` forbids
  on the REST path.
* `theStreamSessionResumesAfterTheRetiredSocketDrops` PASSES: the client
  re-dials the same address, a second handshake is recorded, frames flow again
  and the session reports connected. Recovery works; the window does not.

### F4 — the fake cannot express the clause at all (§10)

`FakeBackend::attach()` returns early for an identical endpoint
(`fake_backend.cpp:496-499`): no bump, no invalidate. Its coalescing is
generation-guarded (`:171-176`), so it could never reproduce F1 either.
`theFakeExpressesASameAddressReplacement` fails. Consequence for the fixer:
none of F1–F3 can be written as a shared `tests/contracts/backend/common/`
case until the fake implements same-address replacement. No existing shared
case covers it (`backend_common_cases.cpp` has 15 cases; the only provider one
is `aDuplicateProviderRequestIsCoalesced`, same-generation only).

## Inversion / satisfiability evidence (all on the external copy, restored after)

| id | mutation | result |
| --- | --- | --- |
| I1 | `mihomo_client.cpp` disconnected handler: drop `reconnect()` | SURVIVED — `logs/inversion-i1.txt`. The `errorOccurred` handler re-dials on an abortive drop, so the two calls are redundant on this path. Recorded, not papered over. |
| I1b | drop `reconnect()` in BOTH the disconnected and errorOccurred handlers | DETECTED — `logs/inversion-i1b.txt`: `theStreamSessionResumesAfterTheRetiredSocketDrops` fails with "the client did not re-dial the same address". The one PASS in this suite has teeth. |
| I2 | `provider_client.cpp:37`: subscribe to `MihomoClient::invalidating` instead of `endpointChanged` (one line) | `logs/inversion-i2.txt` + `logs/inversion-i2b.txt`: all three F1 cases flip to PASS. F2/F3/F4 keep failing, so the suite separates the three roots instead of measuring one artefact. |

I2 is a satisfiability probe, not a prescription: it shows the F1 assertions are
reachable and that the producer/consumer mismatch is their cause. No fix is
proposed on this lease and no repository file was written.

## Reproduction

```
cmake -S snap -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON \
      -DCLASH_QT_BUILD_APP=OFF -Dyaml-cpp_DIR=/opt/homebrew/opt/yaml-cpp/lib/cmake/yaml-cpp
cmake --build build --target transport-audit-tests
python3 ../run_check.py transport-audit build/tests/transport_audit/transport-audit-tests
```

No real engine, no privileged helper, no Clash Verge discovery, no system
network state: a loopback fixture and an explicitly nonexistent engine path
(`<isolated data dir>/no-such-engine`) are the only transports used.
