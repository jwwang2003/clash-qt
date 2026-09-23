#!/usr/bin/env python3
"""A bundle may not claim to support an OS it cannot launch on.

A Mach-O image records the oldest macOS it was built for. The application's own
number is chosen by CMAKE_OSX_DEPLOYMENT_TARGET, but the frameworks it links
carry their own, and the real floor is the highest number in the whole bundle --
dyld refuses to load an image that needs a newer system than the one running.

Nothing in the ordinary build notices when those disagree. The application
compiles, links, launches on the machine that built it, and reports whatever
Info.plist claims; the mismatch appears only on a user's older machine, as a
launch failure with no useful message. That is the worst possible place to find
it, so this check finds it at build time instead.

The usual cause is a Qt obtained from a package manager, which is built for
whatever macOS was current when the bottle was poured. Distributing to older
systems needs a Qt built for the target, not a lower number written here.

    python3 check_bundle_minimum.py --bundle path/to/clash-qt.app [--declared 14.0]
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

MINOS = re.compile(r"^\s*minos\s+([0-9]+(?:\.[0-9]+)*)", re.MULTILINE)


def version(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in text.split("."))


def macho_minimums(bundle: Path) -> dict[Path, tuple[int, ...]]:
    """Every Mach-O in the bundle, mapped to the oldest macOS it accepts."""
    found: dict[Path, tuple[int, ...]] = {}
    for path in bundle.rglob("*"):
        if not path.is_file() or path.is_symlink():
            continue
        with path.open("rb") as handle:
            magic = handle.read(4)
        # Thin and fat Mach-O, both byte orders.
        if magic not in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe",
                         b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca"):
            continue
        out = subprocess.run(["otool", "-l", str(path)],
                             capture_output=True, text=True).stdout
        versions = [version(m) for m in MINOS.findall(out)]
        if versions:
            found[path] = max(versions)
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bundle", required=True, type=Path)
    ap.add_argument("--declared", help="the version the bundle claims, e.g. 14.0")
    args = ap.parse_args()

    bundle = args.bundle.resolve()
    if not bundle.exists():
        print(f"bundle minimum: {bundle} does not exist")
        return 1

    minimums = macho_minimums(bundle)
    if not minimums:
        print(f"bundle minimum: no Mach-O images found under {bundle}")
        return 1

    required = max(minimums.values())
    declared = version(args.declared) if args.declared else required

    if required > declared:
        claimed = ".".join(str(p) for p in declared)
        actual = ".".join(str(p) for p in required)
        print(f"bundle minimum: this bundle claims macOS {claimed} "
              f"but cannot launch below macOS {actual}\n")
        for path, needed in sorted(minimums.items(), key=lambda kv: kv[1], reverse=True):
            if needed > declared:
                print(f"  needs {'.'.join(str(p) for p in needed):<8} "
                      f"{path.relative_to(bundle)}")
        print("\nEvery image above was built for a newer system than this bundle"
              "\nclaims to support. Users on the versions in between will see a"
              "\nlaunch failure, not a message. Build against a Qt whose"
              "\ndeployment target is at or below the version you intend to"
              "\nsupport, or state the higher version as the real requirement.")
        return 1

    print(f"bundle minimum: {'.'.join(str(p) for p in required)}, "
          f"{len(minimums)} images checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
