#!/usr/bin/env python3
"""clash-qt architecture checker.

Reads the *evaluated* CMake target graph through the CMake File API (codemodel
v2) -- not by parsing CMakeLists.txt -- and checks it against the declared
module graph in architecture.json.  A project-include scan is layered on top as
a supplement.

What the File API gives us, and why each field is used:

  linkLibraries / interfaceLinkLibraries
      The evaluated link interface of a target, generator expressions already
      resolved, with *imported* targets (Qt6::Widgets, yaml-cpp::yaml-cpp)
      listed by id alongside project targets.  This is the authoritative set of
      declared edges: it includes edges added from any .cmake file, any
      function, or any generator expression, and it is what CMake actually
      used.
  dependencies
      Link edges plus add_dependencies() edges plus generated build edges
      (AUTOMOC/AUTORCC/QML helper targets).  Used so a generated edge cannot
      hide a cycle.
  compileGroups[].includes / .frameworks
      The include path each source was actually compiled with.  This is the
      *transitive* closure of usage requirements -- a Qt module that arrives
      indirectly through some intermediate target shows up here even though it
      appears in nobody's linkLibraries.  This is how umbrella creep is caught.

Exit status: 0 when every rule passes, 1 when any rule fails or the build
tree cannot be read.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from pathlib import Path

CLIENT = "client-clash-arch"

# --------------------------------------------------------------------------
# Violation codes.  Every rule that can fail has its own code so a self-test can
# assert that *that* rule fired, rather than that "something went wrong".  A
# code no case can provoke is a rule that has quietly stopped checking anything.
#
# These codes are printed verbatim in failure output and are what people grep
# for, so each is a published name: change one only together with the self-test
# in selftest/run_selftests.py that asserts it, or the case silently starts
# accepting a rule that no longer fires.
# --------------------------------------------------------------------------
FORBIDDEN_EDGE = "FORBIDDEN-EDGE"
MODULE_DEPENDS_ON_NON_MODULE = "MODULE-DEPENDS-ON-NON-MODULE"
UNDECLARED_CONSUMER = "UNDECLARED-CONSUMER"
DEPENDENCY_CYCLE = "DEPENDENCY-CYCLE"
FORBIDDEN_EXTERNAL = "FORBIDDEN-EXTERNAL"
UNDECLARED_EXTERNAL = "UNDECLARED-EXTERNAL"
TRANSITIVE_EXTERNAL_CREEP = "TRANSITIVE-EXTERNAL-CREEP"
STALE_EXCEPTION = "STALE-EXCEPTION"
DECLARED_TARGET_MISSING = "DECLARED-TARGET-MISSING"
UNDECLARED_TARGET = "UNDECLARED-TARGET"
EXCEPTION_SITE_TARGET_MISSING = "EXCEPTION-SITE-TARGET-MISSING"
SEALED_MODULE_DECLARED_DEP = "SEALED-MODULE-DECLARED-DEP"
SEALED_MODULE_UNBASELINED_LINK = "SEALED-MODULE-UNBASELINED-LINK"
# A prefix, not a whole code: the emitted code is this joined to the id of the
# include rule that fired, so the failure names the rule rather than the scan
# ("INCLUDE-COMPONENT-PRIVATE", "INCLUDE-CORE-NOT-PLATFORM", ...).
INCLUDE_VIOLATION = "INCLUDE"
STALE_INCLUDE_EXCEPTION = "STALE-INCLUDE-EXCEPTION"


class Violation:
    def __init__(self, code, subject, detail, hint=""):
        self.code = code
        self.subject = subject
        self.detail = detail
        self.hint = hint

    def as_dict(self):
        return {
            "code": self.code,
            "subject": self.subject,
            "detail": self.detail,
            "hint": self.hint,
        }

    def __str__(self):
        line = "%s  %s\n      %s" % (self.code, self.subject, self.detail)
        if self.hint:
            line += "\n      hint: %s" % self.hint
        return line


# --------------------------------------------------------------------------
# CMake File API
# --------------------------------------------------------------------------
def write_query(build_dir: Path) -> Path:
    """Place a codemodel-v2 query so the next configure emits a reply."""
    qdir = build_dir / ".cmake" / "api" / "v1" / "query" / CLIENT
    qdir.mkdir(parents=True, exist_ok=True)
    qfile = qdir / "query.json"
    qfile.write_text(json.dumps({"requests": [{"kind": "codemodel", "version": 2}]}))
    return qfile


def load_codemodel(build_dir: Path):
    """Return (configuration dict, reply dir).  Raises on a missing reply."""
    reply = build_dir / ".cmake" / "api" / "v1" / "reply"
    if not reply.is_dir():
        raise SystemExit(
            "no CMake File API reply in %s\n"
            "  run: python3 arch_check.py --write-query --build-dir %s && cmake -S <src> -B %s"
            % (reply, build_dir, build_dir)
        )
    indexes = sorted(reply.glob("index-*.json"))
    if not indexes:
        raise SystemExit("no File API index in %s (re-run cmake)" % reply)
    index = json.loads(indexes[-1].read_text())

    responses = []
    client = index.get("reply", {}).get(CLIENT, {})
    for value in client.values():
        if isinstance(value, dict) and "responses" in value:
            responses.extend(value["responses"])
        elif isinstance(value, dict) and value.get("kind") == "codemodel":
            responses.append(value)
    # Fall back to the shared reply if the query was placed by someone else.
    if not responses:
        for key, value in index.get("reply", {}).items():
            if key.startswith("codemodel-v2") and isinstance(value, dict):
                responses.append(value)
    codemodels = [r for r in responses if r.get("kind") == "codemodel"]
    if not codemodels:
        raise SystemExit("File API reply has no codemodel response (re-run cmake)")
    doc = json.loads((reply / codemodels[0]["jsonFile"]).read_text())
    return doc, reply


class Target:
    def __init__(self, raw, source_root: Path):
        self.raw = raw
        self.name = raw["name"]
        self.type = raw["type"]
        self.id = raw["id"]
        self.source_root = source_root
        self.direct_deps = set()        # declared link edges to project targets
        self.project_deps = set()       # direct + transitive + generated edges
        self.external_deps = set()      # direct edges to imported targets / flags
        self.transitive_external = set()

    @property
    def is_link_target(self):
        return self.type in (
            "STATIC_LIBRARY",
            "SHARED_LIBRARY",
            "MODULE_LIBRARY",
            "OBJECT_LIBRARY",
            "INTERFACE_LIBRARY",
            "EXECUTABLE",
        )


def _id_to_name(ident: str) -> str:
    # File API target ids look like "Qt6::Qml::@6890427a1f51a3e7e1df".
    return ident.rsplit("::@", 1)[0]


_QT_PATTERNS = (
    re.compile(r"Qt([A-Za-z0-9]+)\.framework"),
    re.compile(r"/include/Qt([A-Za-z0-9]+)(?:/|$)"),
    re.compile(r"libQt6([A-Za-z0-9]+)\.(?:a|so|dylib|lib)"),
    re.compile(r"(?:^|\s)-lQt6([A-Za-z0-9]+)(?:\s|$)"),
)


def external_from_path(path: str):
    """Map an evaluated include dir or link fragment back to an external name."""
    for pattern in _QT_PATTERNS:
        m = pattern.search(path)
        if m:
            return "Qt6::" + m.group(1)
    if re.search(r"(?:^|/)(?:lib)?yaml-cpp", path):
        return "yaml-cpp::yaml-cpp"
    return None


def normalise_fragment(fragment: str):
    fragment = fragment.strip()
    if not fragment:
        return None
    m = re.match(r"-framework\s+(\S+)", fragment)
    if m:
        return "framework:" + m.group(1)
    mapped = external_from_path(fragment)
    if mapped:
        return mapped
    if fragment.startswith("-l"):
        return fragment[2:]
    if fragment.startswith("-"):
        return None  # a plain linker flag, not a dependency
    return fragment


def _referenced_ids(raw):
    out = set()
    for key in ("linkLibraries", "interfaceLinkLibraries", "dependencies"):
        for entry in raw.get(key) or []:
            if isinstance(entry, dict) and "id" in entry:
                out.add(entry["id"])
    return out


def build_graph(codemodel, reply: Path, source_root: Path, config_name=None):
    configs = codemodel["configurations"]
    cfg = configs[0]
    if config_name:
        for candidate in configs:
            if candidate["name"] == config_name:
                cfg = candidate
                break

    raws = []
    for entry in cfg["targets"]:
        raws.append(json.loads((reply / entry["jsonFile"]).read_text()))

    # CMake's codemodel does not list INTERFACE libraries among a
    # configuration's targets -- they have no build rules -- yet they are real
    # project targets and real edges (clash_types is one). Their target
    # objects do exist in the reply, so resolve any referenced id that the
    # configuration did not list, transitively.
    known_ids = {raw["id"] for raw in raws}
    pending = set()
    for raw in raws:
        pending |= _referenced_ids(raw) - known_ids
    extra_files = {}
    if pending:
        for path in reply.glob("target-*.json"):
            try:
                doc = json.loads(path.read_text())
            except (OSError, ValueError):
                continue
            extra_files[doc["id"]] = doc
    # Imported targets (Qt6::Core, yaml-cpp::yaml-cpp) also have target objects
    # in the reply. CMake reserves "::" in a target name for ALIAS and IMPORTED
    # targets, so the name is an exact discriminator: they are externals, and
    # their interface is used only to widen the transitive closure.
    imported_docs = {}
    while pending:
        ident = pending.pop()
        doc = extra_files.get(ident)
        if doc is None or doc["id"] in known_ids:
            continue
        known_ids.add(doc["id"])
        if "::" in doc["name"]:
            imported_docs[doc["name"]] = doc
            continue
        raws.append(doc)
        pending |= _referenced_ids(doc) - known_ids

    targets = {}
    by_id = {}
    for raw in raws:
        t = Target(raw, source_root)
        targets[t.name] = t
        by_id[t.id] = t

    project_names = set(targets)
    # Link fragments naming a project's own build artifacts are internal edges
    # that linkLibraries already covers; they must not be read as externals.
    own_artifacts = set()
    for t in targets.values():
        if t.raw.get("nameOnDisk"):
            own_artifacts.add(t.raw["nameOnDisk"])
        for art in t.raw.get("artifacts") or []:
            own_artifacts.add(Path(art.get("path", "")).name)

    for t in targets.values():
        entries = []
        entries.extend(t.raw.get("linkLibraries") or [])
        entries.extend(t.raw.get("interfaceLinkLibraries") or [])
        for entry in entries:
            if "id" in entry:
                name = _id_to_name(entry["id"])
                if name in project_names:
                    if name != t.name:
                        t.direct_deps.add(name)
                        t.project_deps.add(name)
                else:
                    t.external_deps.add(name)
            elif "fragment" in entry:
                norm = normalise_fragment(entry["fragment"])
                if norm:
                    t.external_deps.add(norm)

        # add_dependencies()/generated edges: kept for cycle detection so a
        # generated target cannot close a loop invisibly.  UTILITY helpers
        # (AUTOMOC, QML tooling) are build-order scaffolding, not architecture.
        for entry in t.raw.get("dependencies") or []:
            dep = by_id.get(entry["id"])
            if dep is None or dep.name == t.name:
                continue
            if dep.type == "UTILITY":
                continue
            t.project_deps.add(dep.name)

        # Transitive usage requirements, straight off the compile lines.
        for group in t.raw.get("compileGroups") or []:
            for inc in group.get("includes") or []:
                mapped = external_from_path(inc.get("path", ""))
                if mapped:
                    t.transitive_external.add(mapped)
            for fw in group.get("frameworks") or []:
                mapped = external_from_path(fw.get("path", ""))
                if mapped:
                    t.transitive_external.add(mapped)
        link = t.raw.get("link") or {}
        for frag in link.get("commandFragments") or []:
            if frag.get("role") not in ("libraries", "frameworks"):
                continue
            text = frag.get("fragment", "").strip()
            if Path(text.strip('"')).name in own_artifacts:
                continue
            mapped = normalise_fragment(text)
            if mapped and mapped not in project_names:
                t.transitive_external.add(mapped)

    # An imported target's own link interface, so a Qt module that another Qt
    # module re-exports is not silently omitted on a platform where the include
    # path cannot be mapped back to a module name.
    imported_interface = {}
    for name, doc in imported_docs.items():
        deps = set()
        for entry in doc.get("interfaceLinkLibraries") or []:
            if "id" in entry:
                deps.add(_id_to_name(entry["id"]))
            elif "fragment" in entry:
                norm = normalise_fragment(entry["fragment"])
                if norm:
                    deps.add(norm)
        imported_interface[name] = {d for d in deps if d != name}

    def expand_externals(names):
        seen = set()
        queue = list(names)
        while queue:
            name = queue.pop()
            if name in seen:
                continue
            seen.add(name)
            queue.extend(imported_interface.get(name, ()))
        return seen

    # Close the transitive external set over project edges as well, so an
    # INTERFACE library (which has no compile groups of its own) still reports
    # what it hands its consumers.
    changed = True
    while changed:
        changed = False
        for t in targets.values():
            grown = set(t.transitive_external) | set(t.external_deps)
            for dep_name in t.project_deps:
                dep = targets.get(dep_name)
                if dep:
                    grown |= dep.external_deps | dep.transitive_external
            grown = expand_externals(grown) - project_names
            if grown != t.transitive_external:
                t.transitive_external = grown
                changed = True

    return targets


# --------------------------------------------------------------------------
# Graph rules
# --------------------------------------------------------------------------
def find_cycles(targets):
    """Tarjan SCC; returns the non-trivial components and self-loops."""
    index = {}
    low = {}
    on_stack = {}
    stack = []
    result = []
    counter = [0]

    names = sorted(targets)
    for root in names:
        if root in index:
            continue
        work = [(root, iter(sorted(targets[root].project_deps)))]
        index[root] = low[root] = counter[0]
        counter[0] += 1
        stack.append(root)
        on_stack[root] = True
        while work:
            node, it = work[-1]
            advanced = False
            for succ in it:
                if succ not in targets:
                    continue
                if succ not in index:
                    index[succ] = low[succ] = counter[0]
                    counter[0] += 1
                    stack.append(succ)
                    on_stack[succ] = True
                    work.append((succ, iter(sorted(targets[succ].project_deps))))
                    advanced = True
                    break
                if on_stack.get(succ):
                    low[node] = min(low[node], index[succ])
            if advanced:
                continue
            work.pop()
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[node])
            if low[node] == index[node]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack[w] = False
                    comp.append(w)
                    if w == node:
                        break
                if len(comp) > 1:
                    result.append(sorted(comp))
                elif comp[0] in targets[comp[0]].project_deps:
                    result.append(comp)
    return result


class Exceptions:
    """Baselined architecture exceptions, with hit tracking for the ratchet.

    Every entry needs an owner and a removal phase, and every entry must
    actually match something.  An exception that no longer matches is reported
    as stale and fails the check: a baseline that is allowed to rot stops being
    a ratchet.
    """

    REQUIRED = ("id", "kind", "owner", "remove_in", "reason")

    def __init__(self, entries):
        self.entries = [e for e in entries if not str(e.get("id", "")).startswith("_")]
        self.hits = {}
        for e in self.entries:
            for field in self.REQUIRED:
                if field not in e:
                    raise SystemExit(
                        "exception %r is missing required field %r; every "
                        "baselined exception needs an owner and a removal phase"
                        % (e.get("id", "<unnamed>"), field)
                    )
            for i, _ in enumerate(self._sites(e)):
                self.hits[(e["id"], i)] = 0

    @staticmethod
    def _sites(entry):
        if "sites" in entry:
            return entry["sites"]
        return [entry]

    def _match(self, kind, predicate):
        for e in self.entries:
            if e["kind"] != kind:
                continue
            for i, site in enumerate(self._sites(e)):
                if predicate(e, site):
                    self.hits[(e["id"], i)] += 1
                    return e
        return None

    def match_edge(self, frm, to):
        return self._match(
            "target_edge", lambda e, s: s.get("from") == frm and s.get("to") == to)

    def match_external(self, target, external):
        return self._match(
            "external_dependency",
            lambda e, s: s.get("target") == target and s.get("external") == external)

    def match_include(self, rule, path, included):
        return self._match(
            "include",
            lambda e, s: e.get("rule") == rule
            and s.get("file") == path
            and s.get("include") == included)

    def stale(self):
        """[(entry, site, index)] for every baselined site that matched nothing."""
        out = []
        for e in self.entries:
            for i, site in enumerate(self._sites(e)):
                if self.hits[(e["id"], i)] == 0:
                    out.append((e, site, i))
        return out

    def describe_site(self, entry, site):
        if entry["kind"] == "include":
            return "%s -> %s" % (site.get("file"), site.get("include"))
        if entry["kind"] == "target_edge":
            return "%s -> %s" % (site.get("from"), site.get("to"))
        if entry["kind"] == "external_dependency":
            return "%s -> %s" % (site.get("target"), site.get("external"))
        return entry["id"]


def _spec_entries(section):
    return {k: v for k, v in section.items() if not k.startswith("_")}


def check_targets(graph, targets, exceptions, allow_missing):
    violations = []
    modules = _spec_entries(graph["modules"])
    consumers = _spec_entries(graph.get("consumers", {}))
    forbidden_external = set(graph.get("forbidden_external", []))
    ignore_external = set(graph.get("ignore_external", []))
    transitive_scope = graph.get("transitive_scope", ["Qt6::*", "yaml-cpp::*"])

    # An edge that only exists because CMake propagates a declared one is not a
    # separate architectural decision: it is already constrained where it was
    # declared. Edges that are NOT reachable that way -- a direct link, or an
    # add_dependencies()/generated edge -- must each be declared.
    def reachable_from(start):
        seen = set()
        queue = list(targets[start].direct_deps)
        while queue:
            n = queue.pop()
            if n in seen or n not in targets:
                continue
            seen.add(n)
            queue.extend(targets[n].direct_deps)
        return seen

    reachable = {name: reachable_from(name) for name in targets}

    def is_propagated(name, dep):
        return dep not in targets[name].direct_deps and dep in reachable[name]
    for t in targets.values():
        t.external_deps -= ignore_external
        t.transitive_external -= ignore_external

    declared = set(modules) | set(consumers)
    present = {n for n, t in targets.items() if t.is_link_target}

    for name in sorted(declared):
        if name in present:
            continue
        spec = modules.get(name) or consumers.get(name) or {}
        if spec.get("optional"):
            continue
        v = Violation(
            DECLARED_TARGET_MISSING,
            name,
            "declared in architecture.json but absent from the evaluated build graph",
            "the module has not been extracted yet, or the build was configured "
            "with a different option set; pass --allow-missing-targets only "
            "while that is deliberate",
        )
        if allow_missing:
            print("SKIPPED  %s  %s" % (v.code, v.subject), file=sys.stderr)
        else:
            violations.append(v)

    # Every library in the project must be declared.  A new library that nobody
    # added to architecture.json would otherwise be entirely unchecked.
    for name in sorted(present):
        t = targets[name]
        if name in declared:
            continue
        if t.type == "EXECUTABLE":
            continue  # executables are covered by the consumer allowlist below
        if not any(
            _glob_match(name, pattern) for pattern in graph.get("ignore_targets", [])
        ):
            violations.append(
                Violation(
                    UNDECLARED_TARGET,
                    name,
                    "library target is not declared in architecture.json",
                    "add it to modules with its allowed deps, or to ignore_targets",
                )
            )

    for name in sorted(modules):
        spec = modules[name]
        t = targets.get(name)
        if t is None:
            continue
        allowed = set(spec.get("deps", []))
        for dep in sorted(t.project_deps):
            if dep in allowed:
                continue
            if exceptions.match_edge(name, dep):
                continue
            if is_propagated(name, dep):
                continue
            if dep in modules:
                violations.append(
                    Violation(
                        FORBIDDEN_EDGE,
                        "%s -> %s" % (name, dep),
                        "edge is neither in the module's declared deps nor a "
                        "baselined exception",
                        "declare it in architecture.json, or remove the link",
                    )
                )
            else:
                violations.append(
                    Violation(
                        MODULE_DEPENDS_ON_NON_MODULE,
                        "%s -> %s" % (name, dep),
                        "a library depends on a non-module target (%s); libraries "
                        "must not depend on application, UI or test targets"
                        % targets[dep].type
                        if dep in targets
                        else "unknown target",
                        "invert the dependency or move the shared code down a layer",
                    )
                )

        allowed_external = set(spec.get("external", []))
        for ext in sorted(t.external_deps):
            if ext in allowed_external:
                continue
            if exceptions.match_external(name, ext):
                continue
            code = (
                FORBIDDEN_EXTERNAL if ext in forbidden_external else UNDECLARED_EXTERNAL
            )
            violations.append(
                Violation(
                    code,
                    "%s -> %s" % (name, ext),
                    "external dependency is not in the module's declared "
                    "'external' allowlist",
                    "a backend module must not reach for desktop Qt"
                    if code == FORBIDDEN_EXTERNAL
                    else "declare the dependency if it is intended",
                )
            )

        # Transitive creep: what the compiler was actually handed.
        banned_here = forbidden_external - allowed_external
        for ext in sorted(t.transitive_external & banned_here):
            if exceptions.match_external(name, ext):
                continue
            violations.append(
                Violation(
                    FORBIDDEN_EXTERNAL,
                    "%s ~> %s" % (name, ext),
                    "forbidden dependency reaches this module transitively "
                    "(seen on its evaluated compile/link line)",
                    "some target in its link closure exposes it PUBLIC",
                )
            )
        if "transitive_external" in spec:
            allow_t = set(spec["transitive_external"]) | allowed_external
            for ext in sorted(t.transitive_external - allow_t):
                # System libraries that Qt itself re-exports (AppKit, IOKit,
                # WrapAtomic ...) are Qt's internals, not this project's
                # architecture. Only names in transitive_scope are ratcheted.
                if not any(_glob_match(ext, p) for p in transitive_scope):
                    continue
                if exceptions.match_external(name, ext):
                    continue
                violations.append(
                    Violation(
                        TRANSITIVE_EXTERNAL_CREEP,
                        "%s ~> %s" % (name, ext),
                        "dependency acquired transitively, outside the module's "
                        "declared transitive allowlist",
                        "a broad umbrella target in the link closure is leaking it",
                    )
                )

    for name in sorted(consumers):
        spec = consumers[name]
        t = targets.get(name)
        if t is None:
            continue
        allowed = set(spec.get("deps", []))
        for dep in sorted(d for d in t.project_deps if d in modules or d in consumers):
            if dep in allowed:
                continue
            if exceptions.match_edge(name, dep):
                continue
            if is_propagated(name, dep):
                continue
            violations.append(
                Violation(
                    UNDECLARED_CONSUMER,
                    "%s -> %s" % (name, dep),
                    "consumer links a module it is not declared to use",
                    "component-private modules must only be linked by their "
                    "declared consumers",
                )
            )
        banned = set(spec.get("forbidden_external", []))
        if spec.get("headless"):
            banned |= forbidden_external
        for ext in sorted(t.transitive_external & banned):
            if exceptions.match_external(name, ext):
                continue
            violations.append(
                Violation(
                    FORBIDDEN_EXTERNAL,
                    "%s ~> %s" % (name, ext),
                    "headless target reaches a desktop Qt module",
                    "a backend test must build and run without Widgets/Quick/Graphs",
                )
            )

    for comp in find_cycles(targets):
        violations.append(
            Violation(
                DEPENDENCY_CYCLE,
                " -> ".join(comp + [comp[0]]),
                "dependency cycle in the evaluated target graph",
                "CMake permits static-library cycles; the architecture does not",
            )
        )

    return violations


# --------------------------------------------------------------------------
# Ledger integrity rules
#
# These do not ask "is this edge allowed" -- check_targets does that.  They ask
# whether the *ledger itself* still describes reality.  Both exist because a
# baseline that drifts away from the graph stops being a ratchet while still
# printing a reassuring site count.
# --------------------------------------------------------------------------
def check_exception_sites(graph, exceptions):
    """Every baselined site must name a target the project declares.

    A site whose subject is not in modules/ or consumers/ names nothing: the
    target was renamed or deleted and the site was left behind.  Such a site is
    indistinguishable from discharged debt -- it inflates the count in the
    summary while matching nothing -- and the stale rule cannot see it, because
    a subject that is absent from the evaluated graph is deliberately excused
    there (a UI test in a headless build is absent but not dead).  The
    discriminator is the declaration, not the build: a live-but-unbuilt target
    is still declared; a deleted one is not.
    """
    declared = set(_spec_entries(graph["modules"]))
    declared |= set(_spec_entries(graph.get("consumers", {})))
    violations = []
    for entry in exceptions.entries:
        if entry["kind"] == "include":
            continue
        for site in Exceptions._sites(entry):
            for role, subject in (("from", site.get("from")),
                                  ("to", site.get("to")),
                                  ("target", site.get("target"))):
                if subject is None or subject in declared:
                    continue
                violations.append(
                    Violation(
                        EXCEPTION_SITE_TARGET_MISSING,
                        "%s: %s" % (entry["id"],
                                    exceptions.describe_site(entry, site)),
                        "the site's %r target %r is declared nowhere in "
                        "architecture.json (owner %s, removal %s)"
                        % (role, subject, entry["owner"], entry["remove_in"]),
                        "the target was deleted or renamed; delete the site "
                        "too -- a dead site reads as discharged debt and "
                        "understates the ledger",
                    )
                )
    return violations


def _seal_specs(graph):
    for spec in graph.get("sealed_modules", []) or []:
        if str(spec.get("module", "")).startswith("_"):
            continue
        yield spec


def check_seals(graph, targets, exceptions):
    """A sealed module may only be reached through the ledger, never around it.

    ``check_targets`` consults ``deps`` BEFORE it consults exceptions, so a
    target that declares a sealed module in ``consumers[].deps`` is permanently
    allowed: no owner, no removal phase, invisible to the ratchet.  That
    converts migration debt into architecture by editing one line.  While the
    exceptions named in ``sealed_while`` are still open, this rule closes that
    route and, in the other direction, requires that every target which
    actually links the module in the evaluated graph is carried as a baselined
    site.  The ledger therefore derives from the graph rather than from memory,
    and both clauses lift automatically when the last named exception is
    deleted.
    """
    violations = []
    for spec in _seal_specs(graph):
        module = spec["module"]
        patterns = spec.get("sealed_while", [])
        covering = [e for e in exceptions.entries
                    if e["kind"] == "target_edge"
                    and any(_glob_match(e["id"], p) for p in patterns)]
        if not covering:
            continue  # the debt this seal guarded is discharged; seal lifted.

        # Clause 1 -- declaration.  No spec may name the sealed module as a dep.
        for section in ("modules", "consumers"):
            for name, entry in sorted(_spec_entries(graph.get(section, {})).items()):
                if module not in (entry.get("deps") or []):
                    continue
                violations.append(
                    Violation(
                        SEALED_MODULE_DECLARED_DEP,
                        "%s -> %s" % (name, module),
                        "%r declares the sealed module %r in its deps while %s "
                        "is open; deps is consulted before exceptions, so this "
                        "edge would be permanently allowed"
                        % (name, module, ", ".join(e["id"] for e in covering)),
                        "move it to a baselined site under %s, with the owner "
                        "and removal phase the debt already has"
                        % covering[0]["id"],
                    )
                )

        # Clause 2 -- reality.  Every direct linker must be in the ledger.
        baselined = set()
        for e in covering:
            for site in Exceptions._sites(e):
                if site.get("to") == module:
                    baselined.add(site.get("from"))
        exempt = spec.get("exempt_linkers", [])
        for name in sorted(targets):
            t = targets[name]
            if name == module or module not in t.direct_deps:
                continue
            if name in baselined:
                continue
            if any(_glob_match(name, p) for p in exempt):
                continue
            violations.append(
                Violation(
                    SEALED_MODULE_UNBASELINED_LINK,
                    "%s -> %s" % (name, module),
                    "target links the sealed module %r but is carried by no "
                    "baselined site of %s"
                    % (module, ", ".join(e["id"] for e in covering)),
                    "add the site to %s with its owner and removal phase, or "
                    "drop the link; the ledger is derived from this graph"
                    % covering[0]["id"],
                )
            )
    return violations


# --------------------------------------------------------------------------
# Include scan (supplement -- see the note printed in the summary)
# --------------------------------------------------------------------------
def _glob_match(path, pattern):
    regex = "^"
    i = 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            regex += "(?:.*/)?"
            i += 3
        elif pattern.startswith("**", i):
            regex += ".*"
            i += 2
        elif pattern[i] == "*":
            regex += "[^/]*"
            i += 1
        elif pattern[i] == "?":
            regex += "[^/]"
            i += 1
        else:
            regex += re.escape(pattern[i])
            i += 1
    return re.match(regex + "$", path) is not None


def qt_header_index(extra_roots=()):
    """header name -> set of Qt modules that publish it.

    Built from the Qt installation itself rather than a hard-coded class list,
    so <QKeySequence> resolves to Qt6::Gui and <QJSEngine> to Qt6::Qml without
    anyone maintaining a table.  Falls back to a small static map when Qt
    headers cannot be located.
    """
    index = {}
    roots = []
    for root in extra_roots:
        roots.append(Path(root))
    for env in ("QT_INSTALL_HEADERS", "QTDIR"):
        if os.environ.get(env):
            roots.append(Path(os.environ[env]))
    roots += [Path("/opt/homebrew/lib"), Path("/opt/homebrew/include"),
              Path("/usr/local/lib"), Path("/usr/local/include"),
              Path("/usr/include/x86_64-linux-gnu/qt6"), Path("/usr/include/qt6")]

    seen_roots = set()
    for root in roots:
        try:
            root = root.resolve()
        except OSError:
            continue
        if root in seen_roots or not root.is_dir():
            continue
        seen_roots.add(root)
        for entry in sorted(root.iterdir()):
            module = None
            headers = None
            if entry.name.endswith(".framework") and entry.name.startswith("Qt"):
                module = "Qt6::" + entry.name[2:-len(".framework")]
                headers = entry / "Headers"
            elif entry.is_dir() and entry.name.startswith("Qt"):
                module = "Qt6::" + entry.name[2:]
                headers = entry
            if not module or not headers or not headers.is_dir():
                continue
            try:
                for header in headers.iterdir():
                    index.setdefault(header.name, set()).add(module)
            except OSError:
                continue

    if not index:
        for name in ("QWidget", "QApplication", "QMainWindow", "QMessageBox",
                     "QMenu", "QDialog", "QPushButton", "QLabel", "QVBoxLayout",
                     "QHBoxLayout", "QSystemTrayIcon", "QStyle", "QTableWidget",
                     "QTabWidget", "QLineEdit", "QCheckBox", "QComboBox"):
            index.setdefault(name, set()).add("Qt6::Widgets")
        for name in ("QQuickItem", "QQuickWindow", "QQuickView", "QQuickWidget",
                     "QQuickRenderControl"):
            index.setdefault(name, set()).add("Qt6::Quick")
        for name in ("QKeySequence", "QGuiApplication", "QIcon", "QPixmap",
                     "QPainter", "QColor", "QFont"):
            index.setdefault(name, set()).add("Qt6::Gui")
        for name in ("QJSEngine", "QJSValue", "QQmlEngine", "QQmlComponent"):
            index.setdefault(name, set()).add("Qt6::Qml")
    return index


_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]')
_SOURCE_SUFFIXES = (".h", ".hpp", ".hh", ".hxx", ".cpp", ".cc", ".cxx", ".c",
                    ".m", ".mm", ".inl")


def scan_includes(src_root: Path, repo_root: Path):
    """[(repo-relative path, included spelling, line no)] for every source."""
    out = []
    for path in sorted(src_root.rglob("*")):
        if not path.is_file() or path.suffix not in _SOURCE_SUFFIXES:
            continue
        rel = path.relative_to(repo_root).as_posix()
        try:
            text = path.read_text(errors="replace")
        except OSError:
            continue
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for n, line in enumerate(text.splitlines(), 1):
            m = _INCLUDE_RE.match(line)
            if m:
                out.append((rel, m.group(2), n))
    return out


def check_includes(graph, repo_root: Path, src_root: Path, exceptions, qt_index):
    violations = []
    rules = graph.get("include_rules", [])
    if not rules:
        return violations
    forbidden_modules = set(graph.get("forbidden_external", []))
    sites = scan_includes(src_root, repo_root)
    observed = set()

    for rel, included, line in sites:
        for rule in rules:
            if not any(_glob_match(rel, p) for p in rule["applies_to"]):
                continue
            if any(_glob_match(rel, p) for p in rule.get("exempt", [])):
                continue
            if any(_glob_match(included, p) for p in rule.get("allow_includes", [])):
                continue
            reason = None

            for pattern in rule.get("forbid_project_includes", []):
                if _glob_match(included, pattern):
                    reason = "matches forbidden project include pattern %r" % pattern
                    break

            if reason is None and rule.get("forbid_qt_modules"):
                banned = set(rule["forbid_qt_modules"]) or forbidden_modules
                head = included.split("/", 1)[0]
                if head.startswith("Qt"):
                    module = "Qt6::" + head[2:]
                    if module in banned:
                        reason = "uses %s directly" % module
                if reason is None:
                    providers = qt_index.get(included.split("/")[-1])
                    if providers and providers <= banned:
                        reason = "resolves to %s" % ", ".join(sorted(providers))

            if reason is None:
                continue

            observed.add((rule["id"], rel, included))
            if exceptions.match_include(rule["id"], rel, included):
                continue
            violations.append(
                Violation(
                    "%s-%s" % (INCLUDE_VIOLATION, rule["id"]),
                    "%s:%d" % (rel, line),
                    "#include %s -- %s: %s" % (included, reason, rule["message"]),
                    rule.get("hint", ""),
                )
            )
    return violations


# --------------------------------------------------------------------------
def main(argv=None):
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--graph", default=str(here / "architecture.json"))
    parser.add_argument("--build-dir")
    parser.add_argument("--repo-root", default=str(here.parent.parent))
    parser.add_argument("--src-root", default=None,
                        help="source tree to scan (default: <repo-root>/src)")
    parser.add_argument("--config", default=None)
    parser.add_argument("--write-query", action="store_true",
                        help="place the File API query and exit")
    parser.add_argument("--ensure-reply", action="store_true",
                        help="if the build tree has no File API reply yet, place "
                             "the query and re-run cmake once to produce it")
    parser.add_argument("--cmake", default=os.environ.get("CMAKE_COMMAND", "cmake"))
    parser.add_argument("--allow-missing-targets", action="store_true",
                        help="report not-yet-extracted targets as SKIPPED instead "
                             "of failing (migration only)")
    parser.add_argument("--no-include-scan", action="store_true")
    parser.add_argument("--no-target-scan", action="store_true")
    parser.add_argument("--dump", action="store_true",
                        help="print the evaluated graph and exit")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    if args.write_query:
        if not args.build_dir:
            parser.error("--write-query needs --build-dir")
        print(write_query(Path(args.build_dir)))
        return 0

    repo_root = Path(args.repo_root).resolve()
    src_root = Path(args.src_root).resolve() if args.src_root else repo_root / "src"
    graph = json.loads(Path(args.graph).read_text())
    exceptions = Exceptions(graph.get("exceptions", []))

    targets = {}
    qt_roots = []
    if args.build_dir and not args.no_target_scan:
        build_dir = Path(args.build_dir)
        if args.ensure_reply:
            write_query(build_dir)
            if not (build_dir / ".cmake" / "api" / "v1" / "reply").is_dir():
                import subprocess
                subprocess.run([args.cmake, str(build_dir)], check=True,
                               stdout=subprocess.DEVNULL)
        codemodel, reply = load_codemodel(build_dir)
        targets = build_graph(codemodel, reply, repo_root, args.config)
        for t in targets.values():
            for group in t.raw.get("compileGroups") or []:
                for inc in group.get("includes") or []:
                    p = inc.get("path", "")
                    if "Qt" in p:
                        qt_roots.append(str(Path(p).parent.parent))

    if args.dump:
        print(json.dumps(
            {
                name: {
                    "type": t.type,
                    "direct_deps": sorted(t.direct_deps),
                    "project_deps": sorted(t.project_deps),
                    "external_deps": sorted(t.external_deps),
                    "transitive_external": sorted(t.transitive_external),
                }
                for name, t in sorted(targets.items())
                if t.is_link_target
            },
            indent=2))
        return 0

    violations = []
    checked = []
    if targets:
        violations += check_targets(graph, targets, exceptions, args.allow_missing_targets)
        violations += check_exception_sites(graph, exceptions)
        violations += check_seals(graph, targets, exceptions)
        checked += ["module edge allowlist", "acyclicity",
                    "external dependency allowlist", "exception ratchet",
                    "exception-site liveness", "sealed-module seals"]
    if not args.no_include_scan:
        qt_index = qt_header_index(qt_roots)
        violations += check_includes(graph, repo_root, src_root, exceptions, qt_index)
        checked.append("project-include scan")

    # A stale baseline entry is only reported when the rules that could have
    # matched it were actually run, otherwise --no-include-scan (or a build
    # without the app targets) would report every unrun rule as stale.
    ran_targets = bool(targets)
    ran_includes = not args.no_include_scan
    for entry, site, _ in exceptions.stale():
        if entry["kind"] == "include" and not ran_includes:
            continue
        if entry["kind"] != "include" and not ran_targets:
            continue
        if entry["kind"] in ("target_edge", "external_dependency"):
            # A site whose target this configuration does not build (a UI test
            # in a headless build, say) was not observable, so it is not stale.
            # This excuse is what let deleted targets linger as dead sites, so
            # it is paid for by check_exception_sites(), which fails any site
            # naming a target the project no longer declares at all.
            subject = site.get("from") or site.get("target")
            if subject not in targets:
                continue
        code = (STALE_INCLUDE_EXCEPTION if entry["kind"] == "include"
                else STALE_EXCEPTION)
        violations.append(
            Violation(
                code,
                "%s: %s" % (entry["id"], exceptions.describe_site(entry, site)),
                "baselined exception site (%s, owner %s, removal %s) matched "
                "nothing" % (entry["kind"], entry["owner"], entry["remove_in"]),
                "the violation is gone -- delete the site from "
                "architecture.json so it cannot silently come back",
            )
        )

    if args.json:
        print(json.dumps({
            "violations": [v.as_dict() for v in violations],
            "rules_checked": checked,
        }, indent=2))
    else:
        print("architecture check: %s" % Path(args.graph).name)
        for rule in checked:
            print("  checked: %s" % rule)
        for klass, heading in (
            ("architecture", "baselined architecture exceptions"),
            ("migration-baseline", "migration baseline (must shrink to zero)"),
        ):
            group = [e for e in exceptions.entries
                     if e.get("class", "architecture") == klass]
            if not group:
                continue
            print("  %s (%d):" % (heading, len(group)))
            for e in group:
                n = len(Exceptions._sites(e))
                print("    %-38s %2d site(s)  owner=%-9s removal=%s"
                      % (e["id"], n, e["owner"], e["remove_in"]))
        print("")
        for v in violations:
            print(v)
            print("")
        if violations:
            print("FAILED: %d violation(s)" % len(violations))
        else:
            print("PASSED")
        print("")
        print("Scope note: the include scan is a supplement, not a proof. It sees "
              "textual #include lines only -- not template instantiation, not "
              "linker-visible symbol use, not runtime coupling through QObject "
              "signals, Qt meta-object lookup, dynamic properties or the "
              "QCoreApplication property bag. The target rules above read the "
              "evaluated CMake graph, which does cover transitive and generated "
              "edges.")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main())
