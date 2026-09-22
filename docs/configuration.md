# Configuration

How clash-qt turns a profile into the YAML document it hands the engine, and
what a user can change along the way.

**Current revision: `config-r1`.** This document is the single definition of that
revision; `src/core/config/config_composer.h` and `src/core/profiles/profile_store.h`
implement it.

The layer this describes is **additive to the existing profile store**. It did
not replace subscription handling, profile files, the legacy enhancement chain or
the runtime launch path; it gave the part of those that decides *what wins* a
name, a shape and somewhere to be tested.

---

## The composer is pure, and that is the point

`core::config::compose()` is a function of its arguments. **No filesystem, no
network, no process, no `QObject`, no clock, no environment.**

That is not stylistic. The precedence rules below used to be a hundred-line
stretch in the middle of the profile store's runtime builder, reachable only by
generating a file on a worker thread. "What wins, a preset or an override?" could
not be asked without writing to disk, so it was never asked — and the one
question a configuration layer exists to answer went untested. It can now be
asked in one call, from a test, with no side effects to arrange or clean up.

The caller supplies **already-enhanced YAML**. The legacy enhancement chain —
user scripts and merge documents — still lives in `core/config/enhance` and runs
*before* composition, on the snapshotted contents its caller took. Its existing
failure-and-continue semantics and its YAML tags are unchanged; composition
neither reimplements nor supersedes it. Its console output is carried through
`ComposeInput::logs` to `ComposeResult::logs` untouched.

Composition implements no policy or network product of its own. It edits a
document.

## Precedence

Lowest to highest. Each layer sees what the layers below it produced.

1. **Source** — the profile YAML, already through the legacy enhancement chain.
2. **Global presets**, in array order.
3. **The selected profile's presets**, in array order.
4. **Runtime overrides** — a shallow, per-top-level-key map merge.
5. **Defaults the controller fills in** — the TUN block and DNS hijack.
6. **Controller-owned fields**, always reasserted.

**Explicit `false`, explicit zero, an explicitly empty sequence and the order of a
sequence are all meaningful values, never "absent".** A layer that treated an
explicit `false` as unset would silently re-enable something the user turned off.

Array order matters at steps 2 and 3 because presets are edits, not a set: two
presets touching one path compose in the order the document lists them, and the
later one wins.

The one deliberate exception to step 6 is the local proxy port; see
[The mixed-port exception](#the-mixed-port-exception).

### Runtime overrides merge shallowly, on purpose

A map value merges key by key into an existing map; anything else replaces
outright. This reproduces the behaviour stored overrides have always had.
Widening it to a deep merge would silently change what every override already on
a user's disk does, which is not a change a refactor gets to make.

## Presets

A preset is a named, toggleable list of edits. The persisted document is JSON:

```json
{
  "version": 1,
  "global": [ Preset ],
  "profiles": { "<profile-uid>": [ Preset ] }
}
```

```json
Preset := { "id": "stable-id", "name": "display name", "enabled": true,
            "operations": [ Op ] }
Op     := { "op": "merge|replace|remove|prepend|append",
            "path": "/dns/enable",
            "value": <any JSON value> }
```

`version` is 1. A document with no version, a non-integer version, or a version
this build does not write is rejected with a diagnostic rather than
half-adopted. A profile uid with no entry in `profiles` simply has no profile
presets. A preset whose `enabled` is false is skipped entirely.

`id` must be stable, because it is what diagnostics and provenance name. Duplicate
ids within a document are a diagnostic.

### Paths

`path` is an **RFC 6901 JSON pointer over map keys only**: `~0` is a literal `~`
and `~1` is a literal `/`. A pointer that is empty, does not start with `/`, has
an empty token, or carries an invalid `~` escape is refused with a diagnostic.

Missing intermediate maps are **created** for every operation but `remove`, which
is what lets a preset set `/dns/enable` on a profile that has no `dns` block at
all. An intermediate that exists but is *not* a map is never rewritten: that is a
bad parent, and it is diagnosed rather than silently replaced.

### Operations

| Operation | Behaviour |
| --- | --- |
| `merge` | Deeply merges maps; **replaces** sequences. Where either side is not a map, the value replaces. |
| `replace` | Replaces exactly. |
| `remove` | Removes a key. **Idempotent** — removing what is not there is not a problem and is not a diagnostic. |
| `prepend` | Adds rules at the front of `/rules`. |
| `append` | Adds rules at the end of `/rules`. |

`prepend` and `append` are **allowed at `/rules` only**, and their `value` must be
an array of strings. Both constraints are checked when the document is parsed, so
a malformed operation never reaches composition. If `/rules` exists and is not a
sequence, the operation is refused with a diagnostic rather than overwriting it.

`merge` is bounded. A fragment that would nest maps deeper than the recursion
guard is **refused before anything is written**, and refusing it takes the whole
composition down rather than applying part of it — see
[Partial application is fatal](#partial-application-is-fatal). The guard exists
because recursion driven by untrusted input with no floor is how a preset editor
turns into a stack overflow; a JSON fragment's depth is already bounded by the
parser, but the guard does not depend on that remaining true.

## Controller-owned fields

Some fields belong to the application outright, because the application has to be
able to find and talk to the engine it launched. A preset or an override that
aims at one of them is **diagnosed and dropped**, and the value is reasserted
afterwards regardless.

The owned pointers:

- `/external-controller`
- `/secret`
- `/mixed-port`
- `/external-ui`
- `/external-ui-url`
- `/port`
- `/socks-port`
- `/profile/store-selected`

**No untrusted preset can point the controller, the secret or the dashboard
directory anywhere of its choosing.** That is the security property this list
exists for, and it is why reassertion happens after every other layer rather than
before.

`/port` and `/socks-port` are in the list because composition **deletes them
unconditionally** — the application runs a single mixed port. A preset that sets
one is not merely overruled, it is inert, and saying so out loud beats the
silence.

A *preset* aimed at an owned path is an **error** diagnostic and the operation is
dropped. An *override* aimed at one is a **warning**: it is inert anyway, because
these fields are reasserted, but a user who set one should not be left wondering
why nothing happened.

### The mixed-port exception

One owned field a trusted runtime override may choose: **the local proxy port.**
The user picks where their browser points. A preset still cannot reach it.

The override wins when it is an integer in 1..65535 — the profile store's own
validity rule. Anything else cannot be a port, so the application default stands
and the user is told, rather than the engine being handed a number it will
refuse.

This is resolved **inside composition**, not by the caller. It used to be
resolved in the profile store, which left `compose()` answering with the default
port to a caller who had just asked for a different one — published-API behaviour
nobody could have wanted. Supply the application default in
`ControllerFields::mixedPort` and let composition settle the value. A caller that
pre-resolves the override as well gets the same *answer*, but the provenance then
says "controller" where "override" is the truth.

## Defaults the controller fills in

Applied after overrides and before the owned fields are reasserted, and only
where the document has not already spoken:

| Path | Default |
| --- | --- |
| `/tun/enable` | `false` |
| `/tun/auto-route` | `true` |
| `/tun/auto-detect-interface` | `true` |
| `/tun/stack` | `mixed` |

`/tun/dns-hijack` is filled in with `any:53` and `tcp://any:53` **only when it is
absent and `/dns/enable` is true**. DNS interception requires an enabled internal
resolver, and an explicitly empty hijack list is an explicit choice that is
preserved.

A `/tun` key that exists but is not a mapping is an error and composition fails:
there is no sensible way to merge a default into a scalar.

## Diagnostics and provenance

`ComposeResult` carries four things beyond the YAML: `ok`, `diagnostics`,
`provenance` and `logs`.

A **`Diagnostic`** is `{ severity, source, path, message }`. `severity` is
`"error"`, `"warning"` or `"info"`. `path` is the RFC 6901 pointer it concerns,
empty when it concerns no single path. `source` names the producer:

| `source` | Produced by |
| --- | --- |
| `"source"` | the source document — unparseable, not a mapping, unrenderable |
| `"presets"` | document-level parsing and persistence |
| `"preset:<id>"` | parsing one preset's operations |
| `"global:<id>"` | applying a global preset during composition |
| `"profile:<id>"` | applying the selected profile's preset |
| `"override"` | a runtime override |
| `"default"` | a controller-filled default |
| `"controller"` | the owned-field layer |

The split between `"preset:<id>"` and `"global:<id>"`/`"profile:<id>"` is not
noise: the first says a document is malformed, the second says a well-formed edit
could not be applied to *this* profile, and a user acts on those differently.

A **`Provenance`** entry is `{ path, source }` — which layer last wrote a path.
**Only paths some layer above the source actually touched are recorded.** A
provenance entry per key of a four-thousand-proxy profile would be noise, and the
question being answered is "who changed this?", not "what is in the file?". The
root `/` entry carries `ComposeInput::sourceLabel`, which names the bottom of the
stack.

`/mixed-port` is always written by the controller layer, but its provenance names
whoever actually *chose* the value.

## What `ok` does and does not mean

`ok == false` means **no candidate configuration exists** and `yaml` is empty.

`ok == true` means **a document was produced**. It does **not** mean the engine
will accept it. Nothing in this layer validates engine semantics — it is a YAML
composer, and a YAML composer that claimed to know what the engine will refuse
would be lying about the one thing that matters. Engine validation is a separate
step, performed by launching a validation child against the candidate; see
module-api.md section 3.

Tests must distinguish composition success from engine-confirmed success.
Conflating them is how a green suite ships a configuration the engine rejects.

### Partial application is fatal

A refused operation — a protected path, a bad parent, a missing `remove` target —
produces a diagnostic and **the rest still composes**. The other operations are
decisions of their own and are not invalidated by one that could not be applied.

An operation that could only be applied in **part** fails the whole call instead.
Today that is a merge fragment nested deeper than the recursion guard. The
distinction is deliberate: a document missing one refused edit still means what
the remaining layers said, while a **truncated** one is not a weaker version of
what the user asked for — it is a different configuration nobody described, and
handing it to an engine is worse than handing back nothing.

Past a fatal operation, no further diagnostics are produced: they would be about
a configuration that is never going to exist.

### Nothing throws

`compose()` never throws. A YAML exception from any layer becomes an error
diagnostic with `ok == false`. This matters beyond tidiness — the backend
contract forbids an exception unwinding across its published interface
(module-api.md section 9), and the configuration path is where yaml-cpp's
exceptions live.

## The profile store's interface

These names and their fields are what the user interface codes against. **Add
beside them; do not reshape them.**

```
QJsonObject presetDocument() const;
bool        setPresetDocument(const QJsonObject &document);
void        requestEffectiveConfigPreview(const QString &profileUid = {});
QVector<config::Diagnostic> lastPresetDiagnostics() const;

signal void presetsChanged();
signal void effectiveConfigPreviewReady(const core::config::ComposeResult &result);
```

`presetDocument()` returns the document **as persisted: normalised**, so two
saves of equivalent input produce equal objects and the accessor is a fixed
point. It is never invalid — an unreadable document on disk is recovered or
replaced before it is ever returned.

`setPresetDocument()` **validates, then persists atomically**. It returns false
and changes **nothing** — not the in-memory document, not the file, not the
last-good copy — when the document is rejected. It persists what was *parsed*
rather than what was handed in; the two differ only in normalisation, and storing
the parsed form is what makes the accessor a fixed point. On success it emits
`presetsChanged()`. The reasons for any outcome are on
`lastPresetDiagnostics()` and are summarised through the store's error channel.

`requestEffectiveConfigPreview()` composes what the selected profile — or the
named one — *would* produce, on a worker, and answers with
`effectiveConfigPreviewReady`. An empty uid means the current profile.

**The preview is deliberately inert.** It writes no file, seeds nothing, cancels
no runtime generation and starts no backend. Its input is snapshotted on the
owning thread, so the answer belongs to one instant. Results from superseded
requests are **dropped** rather than delivered out of order: a late worker still
finishes, and its result is simply not emitted.

One consequence worth knowing: the runtime path mints and saves a controller
secret when there is none, and a preview must not, because that would be a write.
The preview reports the absence instead of inventing a value.

`ComposeResult` and its members are registered as Qt metatypes, so the preview
signal is legal across a queued connection. Call `registerMetaTypes()` before
connecting; it is idempotent.

Everything here belongs to the store's owning thread.

## Recovery

The rule throughout: **rejecting or failing to read a document must never be a
way to lose the one already stored.**

**Two copies on disk.** Alongside the preset document there is a last-good copy,
written from the bytes of a save that already succeeded — so it is a whole
document, and the newest one known to load. Writing it is best effort and
deliberately unchecked: the primary save has already succeeded, so a failure
there costs the next recovery, not this save.

**Loading**, in order:

- Primary readable → use it.
- Primary missing or unusable, last-good readable → recover from it, with an
  explicit warning naming what happened and saying the next save restores both
  copies.
- Neither copy usable, but presets are already loaded → **keep them**. They are
  the user's, they are still valid, and a load was asked to load a document, not
  to throw one away because a disk answered badly. A save made from that state
  writes them back over both files.
- Nothing anywhere and nothing in memory → the empty document. This is a genuine
  first run and there is nothing to report.

Every outcome but the first and the last leaves a reason on
`lastPresetDiagnostics()` and reports through the store's error channel.

**Runtime composition failure never overwrites the last usable runtime file and
never emits a successful candidate.** A failed composition produces no candidate
at all — `ok` is false and `yaml` is empty — which is exactly what keeps the last
usable runtime file where it is.

**Engine-validation recovery is a different mechanism and stays where it is.** A
candidate that composes cleanly may still be rejected by the engine, and
preserving the live core across that rejection belongs to the runtime coordinator
and the backend's validation path (module-api.md sections 3 and 4). This layer
does not claim it, and a test must not use a composition success to stand in for
it.

See architecture.md for where the configuration layer sits, and testing.md for
which lanes cover it.
