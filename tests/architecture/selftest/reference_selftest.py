#!/usr/bin/env python3
"""Self-tests for the reference checker.

Every rule gets a case it must reject and a case it must accept. A rule with no
failing case is not a rule, it is a green light -- the same standard the
architecture self-tests hold, and for the same reason: this checker's whole
value is that it fails when a reference goes stale, so a version of it that
quietly scans nothing would be indistinguishable from success.

One case is here for a specific reason. `subdir-build-file` puts the violation
in a subdirectory CMakeLists.txt, because a scan driven by the glob
`CMakeLists.txt` matches only the repository root and `*.cmake` never matches
`CMakeLists.txt` at all. A scan built that way reports this repository clean
while a real dangling reference sits in a subdirectory build file. That happened.
The case exists so it cannot happen again silently.

    python3 reference_selftest.py [--work-dir DIR] [--verbose]
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
CHECKER = HERE.parent / "reference_check.py"

REFACTOR = ".refactor"

# name -> (files, expect_failure, must_mention)
CASES: dict[str, tuple[dict[str, str], bool, str]] = {
    "resolves": (
        {
            "docs/module-api.md": "# The contract\n",
            "src/backend.h": "// Contract: docs/module-api.md.\n",
        },
        False, "",
    ),
    "missing-doc": (
        {"src/backend.h": "// Contract: docs/module-api.md.\n"},
        True, "does not exist",
    ),
    "subdir-build-file": (
        {
            "src/core/CMakeLists.txt": "# see docs/module-api.md\n",
        },
        True, "src/core/CMakeLists.txt",
    ),
    "names-working-state": (
        {
            f"{REFACTOR}/DECISIONS.md": "# D1\n",
            "src/core/CMakeLists.txt": f"# see {REFACTOR}/DECISIONS.md\n",
        },
        True, "the merge deletes",
    ),
    "names-planning-record": (
        {
            "docs/BUILD_RELEASE_PLAN.md": "# plan\n",
            "scripts/build.cmake": "# see docs/BUILD_RELEASE_PLAN.md\n",
        },
        True, "planning record",
    ),
    "published-doc-is-fine": (
        {
            "docs/build.md": "# Build\n",
            "scripts/build.cmake": "# see docs/build.md\n",
        },
        False, "",
    ),
    "plan-label-in-comment": (
        {"src/core/CMakeLists.txt": "# Component-private per G2: do not link.\n"},
        True, "position in the plan",
    ),
    "plan-label-in-code-is-not-checked": (
        # The rule is about prose. An identifier that happens to contain such a
        # pair is a naming question, not a stale-reference question, and
        # flagging it would make the rule unusable.
        {"src/thing.cpp": 'const char *k = "G2";\nint w05_count = 0;\n'},
        False, "",
    ),
    "markdown-heading-is-prose": (
        # `#` opens a heading, not a comment. Reading only what follows a
        # comment marker would exempt every line of every document.
        {"docs/guide.md": "# G1 -- the first goal\n"},
        True, "position in the plan",
    ),
    "hex-and-units-are-not-labels": (
        {"src/thing.cpp": "// 0xD3 at 16px, D3D path, W3C spec, R8G8B8\n"},
        False, "",
    ),
    "untracked-is-out-of-scope": (
        # Written but never added. The scan is driven by git ls-files, so this
        # must not be read -- the boundary is deliberate, not an oversight.
        {},
        False, "",
    ),
}


def build_repo(root: Path, files: dict[str, str]) -> None:
    root.mkdir(parents=True, exist_ok=True)
    run = lambda *a: subprocess.run(a, cwd=root, check=True, capture_output=True)
    run("git", "init", "-q")
    run("git", "config", "user.email", "selftest@example.invalid")
    run("git", "config", "user.name", "selftest")
    for rel, body in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(body)
    if files:
        run("git", "add", "-A")
        run("git", "commit", "-q", "-m", "case")
    # Written only after the commit, so it is present on disk and absent from
    # the index. Its reference is dangling and must still go unreported: the
    # scan follows the index, not the filesystem.
    (root / "untracked.md").write_text("# names docs/nothing-here.md\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--work-dir", type=Path)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    work = args.work_dir or Path(tempfile.mkdtemp(prefix="reference-selftest-"))
    if work.exists() and args.work_dir:
        shutil.rmtree(work)
    work.mkdir(parents=True, exist_ok=True)

    failures: list[str] = []
    for name, (files, expect_failure, mention) in CASES.items():
        root = work / name
        build_repo(root, files)
        proc = subprocess.run(
            [sys.executable, str(CHECKER), "--repo-root", str(root)],
            capture_output=True, text=True,
        )
        failed = proc.returncode != 0
        out = proc.stdout + proc.stderr
        if failed != expect_failure:
            want = "reject" if expect_failure else "accept"
            failures.append(f"{name}: expected the checker to {want}, it did not\n{out}")
        elif expect_failure and mention not in out:
            failures.append(
                f"{name}: rejected, but not for the stated reason "
                f"(no {mention!r} in output)\n{out}"
            )
        elif args.verbose:
            print(f"  ok  {name}")

    if failures:
        print(f"reference self-test: {len(failures)} of {len(CASES)} case(s) failed\n")
        for f in failures:
            print(f)
        return 1
    print(f"reference self-test: {len(CASES)} cases pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
