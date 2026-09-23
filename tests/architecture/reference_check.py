#!/usr/bin/env python3
"""Every documentation reference in the shipping tree must resolve.

This check exists because of a defect class this project keeps producing: a
producer is moved or removed and a consumer is left behind, still naming it.
Six instances are on record. Most were found by a human reading a diff, because
nothing detects a reference that merely names a file -- a comment citing a
document compiles exactly as well when the document is gone.

Four rules. The first two are about naming something that still exists, the
third about not depending on the plan at all, and the fourth about where a
version number is allowed to live:

  RESOLVES   A path named in a shipping file must exist. A comment that points
             at a missing document is worse than no comment: it costs the
             reader a search before they learn there is nothing to find.

  SURVIVES   A shipping file must not name a path that the merge to the
             published branch deletes. `.refactor/` is working state and the
             SCREAMING_CASE documents under docs/ are planning records; both go.
             A reference to either is dangling already, just not yet visibly.

  NAMES THE   A comment may not identify anything by its position in the
  SUBJECT     refactor plan -- no goal, worker, phase or decision ordinal. Those
              labels mean nothing to someone reading the finished code, and they
              go stale the moment the plan ends: a comment saying a migration
              "cannot start" without some library is actively misleading once
              the migration is over. Say what the thing is and why it is there.

  ONE         A file does not declare which revision of a contract it
  DECLARATION implements. Thirty headers once opened with "Contract:
              docs/module-api.md revision backend-r4", so revising the contract
              silently made thirty files wrong and nothing said so. Name the
              document and the section -- both stable -- and let the document
              state its own revision.

              This is narrower than it first appears, and deliberately so.
              Citing a rule by the revision that introduced it ("backend-r2 A1
              requires the post-bump value") is provenance, it stays true
              forever, and it is allowed. Only the declaration form is banned:
              a Contract: or Specification: line claiming a revision for the
              file that carries it.

The scan is driven by `git ls-files`, never by globs. A pathspec like
`CMakeLists.txt` matches only the root file and `*.cmake` never matches
`CMakeLists.txt` at all, so glob-driven scans of this repo have reported a clean
tree while a real violation sat in a subdirectory build file.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Directories that exist only while the refactor runs.
NON_SURVIVING_DIRS = (".refactor/",)

# Trees that are not ours to police. The self-test tree is excluded because its
# whole purpose is to hold the violations this checker must catch: a dangling
# path and a plan label are fixture data there, not defects. Scanning it would
# make the checker fail on its own evidence, and the only way to make it pass
# would be to weaken the cases until they no longer prove anything.
SKIP_PREFIXES = ("3rdparty/", ".refactor/", "tests/architecture/selftest/")

# Binary and generated things a text scan should not open.
SKIP_SUFFIXES = (".png", ".icns", ".qrc", ".ico", ".jpg", ".svg", ".pdf",
                 ".zip", ".gz", ".mo", ".qm", ".lock", ".sum")

# A path-like token: at least one directory separator and a known text suffix.
REFERENCE = re.compile(
    r"(?<![\w./-])((?:docs|scripts|src|tests|.refactor)/[\w./-]*\.(?:md|py|json|cmake|txt))"
)

# A planning record: all-caps stem under docs/. The published set is lowercase.
PLANNING_DOC = re.compile(r"^docs/[A-Z0-9_]+\.md$")

# A plan-position label: a single letter for the axis (goal, worker, phase,
# decision) and an ordinal, standing alone as a word. R is deliberately absent:
# r1..r4 are the contract's own revision shorthand, which prose may cite, and
# the revision rule below governs where a revision may be DECLARED. Anchored on
# non-word characters rather than \b, because \b would also match inside
# identifiers like `clash_p1_helper`, and because git's own regex engine does
# not implement \b at all -- a scan written with it silently matched nothing.
#
# Lower case counts, and so does a label used as a name fragment: the first
# version of this rule matched only upper case and refused any trailing hyphen,
# so a lower-case label used as a directory-name prefix in a shipping README
# went unreported by the very check written to find it. A label is no less a
# label for being part of a name.
PLAN_LABEL = re.compile(
    r"(?<![\w-])([GWPDgwpd][0-9]{1,2})(?=-[A-Za-z]|[^\w-]|$)"
)

# A contract revision token, e.g. backend-r4, component-r1, config-r1, module-r1.
REVISION = re.compile(r"(?<![\w-])((?:backend|component|config|module)-r[0-9]+)(?![\w-])")

# The documents allowed to declare a revision: the ones that define them.
REVISION_DEFINING = ("docs/module-api.md", "docs/configuration.md")

# A line declaring what contract the file implements, as opposed to prose
# citing a rule. Only this form may not carry a revision.
DECLARATION = re.compile(r"(?:Contract|Specification)\s*:", re.IGNORECASE)

# Contexts where those letter-digit pairs are ordinary text, not plan labels.
PLAN_LABEL_EXEMPT = re.compile(
    # Percentiles read exactly like phase labels and are not one; p95 in a
    # latency comment is the ordinary way to write it.
    r"(0x[0-9A-Fa-f]*|[0-9]+ ?(?:px|pt|dpi)|[pP](?:50|75|90|95|99)"
    r"|D3D|W3C|R[0-9]+G[0-9]+B)"
)

# In source and build files only the comment is prose, so only the comment is
# checked: a plan label inside a string literal or an identifier is a different
# problem, and flagging code would make the rule unusable. In Markdown the whole
# file is prose and `#` opens a heading rather than a comment, so the line is
# read whole. Getting this backwards would treat every Markdown heading as a
# comment and every code fence as exempt.
COMMENT = re.compile(r"(?:#|//|<!--)\s*(.*)$")


def prose_of(rel: str, line: str) -> str:
    if rel.endswith(".md"):
        return line
    match = COMMENT.search(line)
    return match.group(1) if match else ""


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "ls-files", "-z"],
        cwd=root, check=True, capture_output=True, text=True,
    ).stdout
    return [p for p in out.split("\0") if p]


def scan(root: Path) -> list[str]:
    violations: list[str] = []
    for rel in tracked_files(root):
        if rel.startswith(SKIP_PREFIXES) or rel.endswith(SKIP_SUFFIXES):
            continue
        path = root / rel
        try:
            text = path.read_text(encoding="utf-8")
        except (UnicodeDecodeError, OSError):
            continue
        for lineno, line in enumerate(text.splitlines(), 1):
            # A Markdown link names its target twice, once as text and once as
            # the destination. One line, one report.
            for ref in dict.fromkeys(REFERENCE.findall(line)):
                where = f"{rel}:{lineno}"
                if ref.startswith(NON_SURVIVING_DIRS):
                    violations.append(
                        f"{where}: names {ref}, which the merge deletes"
                    )
                    continue
                if PLANNING_DOC.match(ref):
                    violations.append(
                        f"{where}: names {ref}, a planning record the merge deletes"
                    )
                    continue
                if not (root / ref).exists():
                    violations.append(f"{where}: names {ref}, which does not exist")

            prose = prose_of(rel, line)
            if rel not in REVISION_DEFINING and DECLARATION.search(prose):
                for token in dict.fromkeys(REVISION.findall(prose)):
                    violations.append(
                        f"{rel}:{lineno}: declares revision {token}; name the "
                        f"document and section, and let it state its own revision"
                    )
            body = PLAN_LABEL_EXEMPT.sub("", prose)
            for label in PLAN_LABEL.findall(body):
                violations.append(
                    f"{rel}:{lineno}: comment says {label}, a position in the "
                    f"plan rather than a property of the code"
                )
    return violations


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo-root", required=True, type=Path)
    args = ap.parse_args()

    root = args.repo_root.resolve()
    violations = scan(root)
    if violations:
        print(f"reference check: {len(violations)} dangling reference(s)\n")
        for v in violations:
            print(f"  {v}")
        print(
            "\nEither restore what the reference names, or update the reference."
            "\nA reference must point at something that still exists after the"
            "\nmerge to the published branch."
        )
        return 1
    print(f"reference check: all references resolve ({len(tracked_files(root))} files scanned)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
