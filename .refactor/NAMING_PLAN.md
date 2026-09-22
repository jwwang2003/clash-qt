# Naming normalisation — the wave between P4 and P5

Process artifact. Like everything in `.refactor/`, it does not reach `main`.

## Why, and why here

Comments and identifiers across the tree cite process vocabulary: goal ids
(`G1`–`G8`), workflow ids (`W01`–`W08`), feature ids (`F01`–`F13`), decision ids
(`D1`–`D8`), contract revisions, wave and package names (`MOD-CORE`, `PRE-ARCH`).

**These become dangling pointers at cutover.** `docs/` and `.refactor/` are both
removed from the merged tree by the binding rule in the progress ledger, so a
comment reading *"DECISION D2: inject an abstract interface"* will point at
nothing a reader can open. That is a correctness problem with a deadline, not a
matter of taste.

**It happens after P4 and not before** because until then the vocabulary is still
doing work: P4's own exit criteria are stated as discharging the `G2-*`
exceptions. Renaming mid-wave would remove the terms being verified against.

Measured before P4 began: **481 references across 57 tracked files** under `src/`
and `tests/` — 147 feature ids, 101 contract revisions, 53 goal ids, 49 workflow
ids, 47 checker codes, 58 package names, 12 decision ids. P4 will add more.

## The principle

**Inline the reasoning; do not merely delete the citation.**

A comment that says *"DECISION D2: inject an abstract interface"* and becomes
*"inject an abstract interface"* is worse than before — the pointer is gone and no
reason replaced it. It must become self-contained:

> The privileged client is injected through an interface this library owns, so no
> platform type appears in the component's published surface.

That is most of the work. Each of the 481 sites needs a judgement about what the
reason actually was, which is why this is a wave and not a find-and-replace.

## The descriptive name usually already exists

The opaque part is a prefix on a name that is already good:

| Today | Becomes |
| --- | --- |
| `w01-first-launch` | `first-launch` |
| `w03-routing-controls` | `routing-controls` |
| `w04-restore`, `w05-recovery` | `restore`, `recovery` |
| `F03 Configuration` | `configuration` |
| `G2-consumers-link-component-private` | `consumers-link-component-private` |
| `ARCH-R5-INCLUDE-IR-COMPONENT-PRIVATE` | `INCLUDE-COMPONENT-PRIVATE` |

So for most identifiers the change is dropping a numeric prefix and updating
whatever matches it — not inventing new vocabulary.

## Three identifiers are machine-matched; renaming them without their matcher
## silently breaks a mechanism

1. **Exception ids** are matched by `sealed_while` **globs** in
   `tests/architecture/architecture.json`. The `G2-` prefix is load-bearing: rename
   the id alone and the seal stops applying, with nothing failing to say so.
   Rename id and glob together, then prove the seal still bites by adding a
   violating edge.
2. **CTest names** encode workflow ids (`w01-first-launch`). Renaming is a test
   rename: update `tests/README.md`'s map in the same change, and keep the
   per-suite case counts identical across it.
3. **Checker rule codes** (`ARCH-R*`) are emitted in failure output and asserted
   by the checker's own self-tests. Rename code and self-test together, and
   confirm each rule still fails on its violating graph.

## What survives rather than being genericised

**Contract revisions.** `backend-r4` and `component-r1` are real versioning, not
bookkeeping — they say which revision a header implements. `docs/module-api.md` is
already in the planned cutover doc set, so the contracts move there and the
citations keep meaning something. Point them at their new home; do not delete them.

## Execution — three workers, disjoint by file

- **NAME-SRC** — `src/**` comments. Inline reasoning, drop process vocabulary.
  No code, no signature, no behaviour change; prove it by showing the diff with
  comment lines filtered is empty.
- **NAME-TESTS** — `tests/**` comments plus the CTest renames, with
  `tests/README.md` updated in the same change. Per-suite case counts identical
  before and after.
- **NAME-CHECKER** — `tests/architecture/**`: exception ids with their globs,
  rule codes with their self-tests. Every rule proven still to fail on its
  violating graph.

Coordinator holds central build files and applies registrations, as always.

## Done when

- No comment under `src/` or `tests/` cites a file that cutover removes.
- The architecture lane is green, and each renamed rule has been shown to still
  fail on a violating graph.
- Per-suite case counts are unchanged; `tests/README.md` matches the registered
  names exactly.
- A grep for the old vocabulary returns only the contract revisions, which are
  intentional and point at `docs/module-api.md`.

## Convention for P5 and beyond

Adopted now, so later waves stop adding references this pass would have to unwind:

> **A comment explains itself.** State the reason in the comment. Do not cite a
> goal, decision, workflow, feature or package identifier as the explanation. If
> the reason is worth a reader's time, it is worth a sentence; if it is not, the
> comment can go. Identifiers may appear only where a machine matches them — a
> test name, an exception id, a checker code, a contract revision — and there the
> identifier is the mechanism, not the explanation.
