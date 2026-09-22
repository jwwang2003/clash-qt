#!/usr/bin/env python3
"""Self-tests for the architecture checker.

Every rule the checker implements has a pair of tiny synthetic projects here:
one the rule must accept and one it must reject. A rule with no failing case
is not a rule -- it is a green light. These self-tests are what stop
arch_check.py from quietly degrading into a checker that passes because it
looks at nothing.

Each case declares the exact violation codes it expects, so a case cannot pass
by failing for the wrong reason.  Those codes are the ones arch_check.py emits:
rename a code there without renaming it here and the case stops asserting
anything about the rule it is named for, which is the one failure mode these
self-tests exist to rule out.

    python3 run_selftests.py [--work-dir DIR] [--filter NAME] [--verbose]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CASES = HERE / "cases"
CHECKER = HERE.parent / "arch_check.py"


# --------------------------------------------------------------------------
# name, kind, project/src, graph, expected codes ([] means "must pass")
# --------------------------------------------------------------------------
GRAPH_CASES = [
    {
        "name": "allowed-graph",
        "project": "allowed",
        "graph": "allowed/architecture.json",
        "expect": [],
        "why": "The reference shape. Qt Gui in a platform adapter and Qt Qml in "
               "a core library are both allowed -- only Widgets, Quick and "
               "Graphs are desktop UI. Must pass.",
    },
    {
        "name": "forbidden-edge",
        "project": "forbidden_edge",
        "graph": "forbidden_edge/architecture.json",
        "expect": ["FORBIDDEN-EDGE"],
        "expect_subject": "synth_telemetry -> synth_impl",
        "why": "A leaf module linking up into an implementation library.",
    },
    {
        "name": "dependency-cycle",
        "project": "cycle",
        "graph": "cycle/architecture.json",
        "expect": ["DEPENDENCY-CYCLE"],
        "why": "CMake configures a static-library cycle without complaint.",
    },
    {
        "name": "forbidden-external",
        "project": "forbidden_qt",
        "graph": "forbidden_qt/architecture.json",
        "expect": ["FORBIDDEN-EXTERNAL"],
        "expect_subject": "synth_core -> Qt6::Widgets",
        "forbid_subject": ["synth_platform", "synth_config"],
        "why": "Widgets in a backend module fails; Gui and Qml in the same "
               "project do not.",
    },
    {
        "name": "transitive-external-creep",
        "project": "umbrella_creep",
        "graph": "umbrella_creep/architecture.json",
        "expect": ["FORBIDDEN-EXTERNAL", "TRANSITIVE-EXTERNAL-CREEP"],
        "expect_subject": "synth_telemetry ~> Qt6::Widgets",
        "why": "Nothing links Widgets directly; it arrives through an umbrella "
               "target. Only the evaluated closure shows it.",
    },
    {
        "name": "additional-exception-not-free",
        "project": "extra_exception",
        "graph": "extra_exception/architecture.json",
        "expect": ["FORBIDDEN-EDGE"],
        "expect_subject": "synth_yaml -> synth_platform",
        "forbid_subject": ["synth_impl -> synth_platform"],
        "why": "One baselined edge must not make the next one free.",
    },
    {
        "name": "undeclared-external",
        "project": "allowed",
        "graph": "variants/undeclared_external.json",
        "expect": ["UNDECLARED-EXTERNAL"],
        "expect_subject": "synth_config -> Qt6::Qml",
        "why": "An external that is not forbidden is still not free. Only the "
               "forbidden ones have a case of their own, so without this one the "
               "allowlist half of the rule could stop working unnoticed.",
    },
    {
        "name": "module-depends-on-non-module",
        "project": "allowed",
        "graph": "variants/non_module_dep.json",
        "expect": ["MODULE-DEPENDS-ON-NON-MODULE"],
        "expect_subject": "synth_impl -> synth_platform",
        "why": "A library depending on a target that is not a module inverts the "
               "layering. The forbidden-edge rule cannot report it, because that "
               "arm only compares modules against modules.",
    },
    {
        "name": "undeclared-consumer",
        "project": "allowed",
        "graph": "variants/undeclared_consumer.json",
        "expect": ["UNDECLARED-CONSUMER"],
        "expect_subject": "synth_app -> synth_impl",
        "why": "The consumer allowlist is all that stands between a "
               "component-private module and anything that links it, so a "
               "consumer reaching past its own declaration must fail.",
    },
    {
        "name": "stale-exception",
        "project": "allowed",
        "graph": "variants/stale_exception.json",
        "expect": ["STALE-EXCEPTION"],
        "why": "A baseline entry that matches nothing must fail, not linger.",
    },
    {
        "name": "missing-target",
        "project": "allowed",
        "graph": "variants/missing_target.json",
        "expect": ["DECLARED-TARGET-MISSING"],
        "why": "A declared module that is absent must fail by default, so the "
               "checker cannot be green because nothing exists yet.",
    },
    {
        "name": "missing-target-allowed",
        "project": "allowed",
        "graph": "variants/missing_target.json",
        "args": ["--allow-missing-targets"],
        "expect": [],
        "expect_stdout": "SKIPPED  DECLARED-TARGET-MISSING",
        "why": "With the migration flag it passes, but says out loud what it "
               "did not check.",
    },
    {
        "name": "undeclared-target",
        "project": "allowed",
        "graph": "variants/undeclared_target.json",
        "expect": ["UNDECLARED-TARGET"],
        "why": "A library nobody declared must not slip through unchecked.",
    },
    {
        "name": "zombie-exception-site",
        "project": "allowed",
        "graph": "variants/zombie_site.json",
        "expect": ["EXCEPTION-SITE-TARGET-MISSING"],
        "expect_subject": "synth_ghost",
        "why": "A dead site inside a LIVE exception must fail. The stale rule "
               "cannot see it, because it excuses any subject missing from the "
               "evaluated graph; a dead site is otherwise indistinguishable "
               "from discharged debt and silently understates the ledger.",
    },
    {
        "name": "site-target-unbuilt",
        "project": "allowed",
        "graph": "variants/site_target_unbuilt.json",
        "expect": [],
        "why": "The false-positive guard for the rule above: a site naming a "
               "declared consumer that this configuration does not build is "
               "not a zombie. Deleted and not-built-here must stay distinct.",
    },
    {
        "name": "sealed-module-honoured",
        "project": "sealed_module",
        "graph": "sealed_module/architecture.json",
        "expect": [],
        "why": "Every direct linker of the sealed module is carried by a "
               "baselined site, and no deps entry names it. Must pass, or the "
               "seal would just be a ban on the module existing.",
    },
    {
        "name": "sealed-module-declared-dep",
        "project": "sealed_module",
        "graph": "sealed_module/violating_dep.json",
        "expect": ["SEALED-MODULE-DECLARED-DEP",
                   "STALE-EXCEPTION"],
        "expect_subject": "synth_app -> synth_impl",
        "why": "The escape hatch: deps is consulted BEFORE exceptions, so one "
               "line of JSON converts tracked debt into permanent "
               "architecture -- and makes the site that was tracking it read "
               "as discharged. Both halves of that signature must fire.",
    },
    {
        "name": "sealed-module-unbaselined-link",
        "project": "sealed_module",
        "graph": "sealed_module/violating_link.json",
        "expect": ["SEALED-MODULE-UNBASELINED-LINK"],
        "expect_subject": "synth_tool -> synth_impl",
        "forbid_subject": ["synth_probe", "synth_app"],
        "why": "An undeclared executable is invisible to the consumer "
               "allowlist. While the module is sealed, the ledger is derived "
               "from the evaluated graph instead, so a linker it does not name "
               "fails -- and a target that does not link it is left alone.",
    },
]

INCLUDE_CASES = [
    {
        "name": "includes-allowed",
        "src": "include_allowed",
        "graph": "include_allowed/architecture.json",
        "expect": [],
        "why": "False-positive guard for the Qt rule: QJSEngine/QQmlEngine in "
               "core, QKeySequence/QGuiApplication in platform, core/types.h "
               "shared. Qml is not Quick and Gui is not Widgets; a rule written "
               "as 'ban anything QML-ish' or 'ban all of Qt Gui' fails here.",
    },
    {
        "name": "includes-forbidden",
        "src": "include_forbidden",
        "graph": "include_forbidden/architecture.json",
        "expect": [
            "INCLUDE-NO-UPWARD-INCLUDE",
            "INCLUDE-NO-DESKTOP-QT",
            "INCLUDE-PLATFORM-NOT-CORE",
            "INCLUDE-COMPONENT-PRIVATE",
        ],
        "why": "One file per rule.",
    },
    {
        "name": "stale-include-baseline",
        "src": "include_stale",
        "graph": "include_stale/architecture.json",
        "expect": ["STALE-INCLUDE-EXCEPTION"],
        "why": "A baselined include site that no longer exists must fail.",
    },
]


class Failure(Exception):
    pass


def run(cmd, cwd=None, env=None):
    return subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True)


def configure(project: Path, build: Path, verbose):
    build.mkdir(parents=True, exist_ok=True)
    query = build / ".cmake" / "api" / "v1" / "query" / "client-clash-arch"
    query.mkdir(parents=True, exist_ok=True)
    (query / "query.json").write_text(
        json.dumps({"requests": [{"kind": "codemodel", "version": 2}]}))
    r = run(["cmake", "-S", str(project), "-B", str(build)])
    if r.returncode != 0:
        raise Failure("cmake configure failed for %s:\n%s\n%s"
                      % (project.name, r.stdout[-3000:], r.stderr[-3000:]))
    if verbose:
        print(r.stdout)


def check_result(case, proc, payload):
    codes = [v["code"] for v in payload["violations"]]
    expected = case["expect"]

    if expected:
        if proc.returncode != 1:
            raise Failure("expected the checker to FAIL (exit 1) but it exited %d\n"
                          "violations: %s" % (proc.returncode, codes))
        for want in expected:
            if want not in codes:
                raise Failure("expected violation %s, got %s" % (want, codes or "none"))
        unexpected = [c for c in codes if c not in expected]
        if unexpected:
            raise Failure("unexpected extra violations %s (the case must fail for "
                          "the stated reason only)" % sorted(set(unexpected)))
    else:
        if proc.returncode != 0:
            raise Failure("expected the checker to PASS but it exited %d with %s\n%s"
                          % (proc.returncode, codes,
                             json.dumps(payload["violations"], indent=2)[:2000]))

    want_subject = case.get("expect_subject")
    if want_subject:
        subjects = [v["subject"] for v in payload["violations"]]
        if not any(want_subject in s for s in subjects):
            raise Failure("expected a violation about %r, got %s"
                          % (want_subject, subjects))
    for bad in case.get("forbid_subject", []):
        for v in payload["violations"]:
            if bad in v["subject"]:
                raise Failure("must NOT report %r but did: %s" % (bad, v["subject"]))


def run_graph_case(case, work: Path, verbose):
    project = CASES / case["project"]
    build = work / ("build-" + case["project"])
    if not (build / ".cmake" / "api" / "v1" / "reply").is_dir():
        configure(project, build, verbose)

    cmd = [sys.executable, str(CHECKER),
           "--graph", str(CASES / case["graph"]),
           "--build-dir", str(build),
           "--repo-root", str(project),
           "--no-include-scan"] + case.get("args", [])

    human = run(cmd)
    proc = run(cmd + ["--json"])
    if proc.returncode not in (0, 1):
        raise Failure("checker crashed (exit %d):\n%s\n%s"
                      % (proc.returncode, proc.stdout[-2000:], proc.stderr[-2000:]))
    payload = json.loads(proc.stdout)
    if case.get("expect_stdout") and case["expect_stdout"] not in (human.stdout + human.stderr):
        raise Failure("expected %r on stdout, got:\n%s"
                      % (case["expect_stdout"], human.stdout))
    check_result(case, proc, payload)
    return human.stdout


def run_include_case(case, work: Path, verbose):
    root = CASES / case["src"]
    cmd = [sys.executable, str(CHECKER),
           "--graph", str(CASES / case["graph"]),
           "--repo-root", str(root),
           "--src-root", str(root / "src"),
           "--no-target-scan"] + case.get("args", [])
    human = run(cmd)
    proc = run(cmd + ["--json"])
    if proc.returncode not in (0, 1):
        raise Failure("checker crashed (exit %d):\n%s\n%s"
                      % (proc.returncode, proc.stdout[-2000:], proc.stderr[-2000:]))
    check_result(case, proc, json.loads(proc.stdout))
    return human.stdout


def run_probe_case(work: Path, verbose):
    """The public-header probe: good module builds, bad module must not."""
    project = CASES / "probe"
    build = work / "build-probe"
    configure(project, build, verbose)

    out = []
    good = run(["cmake", "--build", str(build), "--target", "arch_probe_synth_good"])
    if good.returncode != 0:
        raise Failure("the self-contained public header failed its probe:\n%s\n%s"
                      % (good.stdout[-3000:], good.stderr[-3000:]))
    out.append("arch_probe_synth_good: built (exit 0)")

    bad = run(["cmake", "--build", str(build), "--target", "arch_probe_synth_bad"])
    if bad.returncode == 0:
        raise Failure("a public header that needs a private header and a prior "
                      "include compiled anyway -- the probe proves nothing")
    text = (bad.stdout + bad.stderr)
    for token in ("private_detail.h", "PrivateHandle", "string"):
        if token in text:
            break
    else:
        raise Failure("the probe failed, but not for the expected reason:\n%s"
                      % text[-3000:])
    out.append("arch_probe_synth_bad: failed to compile (exit %d), as required"
               % bad.returncode)
    out.append(_first_error_lines(text))
    return "\n".join(out)


def _first_error_lines(text, n=6):
    lines = [l for l in text.splitlines() if "error" in l.lower()]
    return "\n".join("    " + l.strip() for l in lines[:n])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--work-dir", default=None)
    parser.add_argument("--filter", default=None)
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args(argv)

    work = Path(args.work_dir).resolve() if args.work_dir else \
        (HERE.parent.parent.parent / "build-arch" / "arch-selftest")
    work.mkdir(parents=True, exist_ok=True)

    plan = []
    for case in GRAPH_CASES:
        plan.append((case["name"], case.get("why", ""),
                     lambda c=case: run_graph_case(c, work, args.verbose)))
    for case in INCLUDE_CASES:
        plan.append((case["name"], case.get("why", ""),
                     lambda c=case: run_include_case(c, work, args.verbose)))
    plan.append(("public-header-probe",
                 "A published header must compile alone against only its "
                 "module's declared dependencies.",
                 lambda: run_probe_case(work, args.verbose)))

    if args.filter:
        plan = [p for p in plan if args.filter in p[0]]

    failures = []
    print("architecture checker self-tests (%d cases)" % len(plan))
    print("work dir: %s" % work)
    print("")
    for name, why, fn in plan:
        try:
            output = fn()
            print("PASS  %s" % name)
            if why:
                print("        %s" % why)
            if args.verbose and output:
                print("\n".join("        | " + l for l in output.splitlines()))
        except Failure as exc:
            failures.append((name, str(exc)))
            print("FAIL  %s" % name)
            print("\n".join("        " + l for l in str(exc).splitlines()))
        except Exception as exc:  # noqa: BLE001 - report, do not mask
            failures.append((name, repr(exc)))
            print("ERROR %s: %r" % (name, exc))

    print("")
    if failures:
        print("%d of %d self-tests failed:" % (len(failures), len(plan)))
        for name, _ in failures:
            print("  %s" % name)
        return 1
    print("all %d self-tests passed" % len(plan))
    print("Each rule was shown to accept its allowed graph and to reject its "
          "forbidden one; a permanently green checker would fail these.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
