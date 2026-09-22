# Docs cleanup — the published set replaces the planning set

## Why now

Two facts force this, and both are already true on HEAD:

1. **32 shipping files cite `docs/module-api.md`. It does not exist.** Every
   contract comment in `src/` and `tests/` points at a file that was only ever
   going to be written "at cutover". That is this project's recurring defect
   class -- a consumer kept, its producer missing -- sitting in the tree today,
   with nothing that detects it.

2. **`docs/` holds 13 planning documents that must not reach `main`.** They
   describe the refactor, not the product. They are the same category as
   `.refactor/`, which is already excluded, and they are stale: they still
   describe a loader that moved and a structure that changed.

Writing the published set now makes the 32 citations resolve today and reduces
cutover to deleting directories.

## The convention this wave follows

Carried forward from the naming wave, and binding on P5 and beyond:

- **Name by subject, not by plan position.** No goal, worker, phase or decision
  ordinal in any shipping file -- not in names, not in comments.
- **A revision token belongs only to the contract that defines it.** Prose may
  cite a rule in a published doc; a file may not claim a revision for itself.
- **A reference must survive cutover.** Nothing under `src/`, `tests/`,
  `scripts/` or the build files may point into `.refactor/` or at a planning doc.

## Promotion, not duplication

The contracts **move**. `.refactor/BACKEND_CONTRACT.md` does not stay behind as a
second definition of `backend-r4`; if it did, two files would define one revision
and the rule above would be broken by the very wave that restates it. After this
wave the published doc is the contract, and P5+ amends it in place.

## The published set

| Document | Replaces | Has code consumers |
|---|---|---|
| `docs/module-api.md` | `BACKEND_CONTRACT`, `COMPONENT_CONTRACT`, ABI contract | yes -- 32 citations |
| `docs/configuration.md` | `P4_CONFIG_CONTRACT` | yes |
| `docs/architecture.md` | `STRUCTURE_REFACTOR_PLAN`, `MODULAR_MANAGEMENT_PLAN` | no |
| `docs/build.md` | `BUILD_RELEASE_PLAN` (build half) | yes -- 1 citation |
| `docs/packaging.md` | `BUILD_RELEASE_PLAN` (ship half) | no |
| `docs/development.md` | `IMPLEMENTATION_ROADMAP` | no |
| `docs/testing.md` | `TEST_STRATEGY` | no |
| `docs/migration.md` | `AUDIT`, `FEATURE_PARITY`, `QCLASH_REFERENCE` | no |
| `docs/service-protocol.md` | renamed from `MACOS_SERVICE_PROTOCOL` | no |

`service-protocol.md` was missing from the first version of this table, which
would have deleted it. It is not a planning record: it specifies the privileged
helper's wire format, including frame limits and the peer-UID check, and nothing
in the new set carries that detail. It is renamed to the convention and kept.
The lesson is that "everything in SCREAMING_CASE is a plan" was a guess about a
naming pattern, not a reading of the contents.

## Done when

- `docs/` contains exactly the eight documents above.
- Every `docs/*.md` path cited from `src/`, `tests/`, `scripts/`, `Makefile` or
  any `CMakeLists.txt` resolves to a file that exists.
- No shipping file cites `.refactor/` or a planning doc.
- Both make lanes and all 59 suites stay green; the check is that documentation
  work touched no build input.
