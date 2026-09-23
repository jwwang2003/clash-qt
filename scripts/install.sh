#!/bin/sh
# Install a staged clash-qt bundle into an Applications directory.
#
# `make package` produces a bundle under the build tree. That bundle is not
# installed: it sits beside the developer build, which is why a machine that has
# packaged a few times shows several copies of the application to Spotlight and
# to Open With. This script is the step that turns one staged bundle into the
# installed application, and it removes the copy it replaces rather than leaving
# it to accumulate.
#
# It deliberately does NOT touch the privileged helper. That is installed by the
# application itself, through an authorization prompt, and replacing the
# application does not replace the helper -- see uninstall.sh and
# docs/installing.md for why that matters after an upgrade.
#
# Usage:
#   scripts/install.sh [--from BUNDLE] [--to DIR] [--force]
#
#   --from BUNDLE  the staged .app to install   (default: build/dev/stage/clash-qt.app)
#   --to DIR       where to install it          (default: /Applications)
#   --force        replace a running or newer installation without asking

set -eu

bundle="build/dev/stage/clash-qt.app"
destination="/Applications"
force=0

while [ $# -gt 0 ]; do
    case "$1" in
        --from) bundle="${2:?--from needs a path}"; shift 2 ;;
        --to) destination="${2:?--to needs a directory}"; shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "install: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

if [ ! -d "$bundle" ]; then
    echo "install: no bundle at $bundle" >&2
    echo "         Run 'make package' first, or pass --from." >&2
    exit 1
fi
if [ ! -x "$bundle/Contents/MacOS/clash-qt" ]; then
    echo "install: $bundle has no executable; it is not a complete bundle." >&2
    exit 1
fi

name="$(basename "$bundle")"
target="$destination/$name"

version="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$bundle/Contents/Info.plist" 2>/dev/null || echo unknown)"

# A bundle replaced while it is running leaves the running process backed by a
# deleted image: it keeps working until it next touches a resource and then
# fails in ways that look like corruption rather than like an interrupted
# upgrade. Refuse instead.
if pgrep -f "$target/Contents/MacOS/clash-qt" >/dev/null 2>&1; then
    if [ "$force" -eq 0 ]; then
        echo "install: $target is running. Quit it first, or pass --force." >&2
        exit 1
    fi
    echo "install: stopping the running application"
    pkill -f "$target/Contents/MacOS/clash-qt" || true
    # Give it a moment to release its files before the replacement lands.
    i=0; while pgrep -f "$target/Contents/MacOS/clash-qt" >/dev/null 2>&1 && [ $i -lt 50 ]; do
        sleep 0.1; i=$((i + 1))
    done
fi

if [ -d "$target" ]; then
    installed="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
        "$target/Contents/Info.plist" 2>/dev/null || echo unknown)"
    echo "install: replacing $installed already at $target"
fi

# Staged beside the destination and moved into place, so an interrupted copy
# never leaves a half-written application where a working one used to be. Both
# paths are on the same filesystem, which is what makes the move atomic.
staging="$destination/.$name.incoming.$$"
rm -rf "$staging"
trap 'rm -rf "$staging"' EXIT INT TERM

# ditto, not cp: it preserves the extended attributes and resource data that a
# bundle's code signature is computed over. `cp -R` can produce a copy that no
# longer verifies, which surfaces later as Gatekeeper refusing to open an
# application that was signed correctly when it was built.
if ! /usr/bin/ditto "$bundle" "$staging" 2>/dev/null; then
    echo "install: cannot write to $destination." >&2
    echo "         Either run this with sudo, or install somewhere you own:" >&2
    echo "           scripts/install.sh --to \"\$HOME/Applications\"" >&2
    exit 1
fi

rm -rf "$target"
mv "$staging" "$target"
trap - EXIT INT TERM

# Gatekeeper judges what is on disk, not what was signed in the build tree, and
# a copy can invalidate a signature. Report rather than assume.
if codesign --verify --deep --strict "$target" 2>/dev/null; then
    signature="signature verified"
else
    signature="SIGNATURE INVALID -- macOS may refuse to open this"
fi

echo "install: clash-qt $version -> $target ($signature)"

# The helper is versioned separately and is not replaced here. Saying so is the
# difference between an upgrade the user understands and one that silently keeps
# an old root-owned component.
if [ -d /Library/PrivilegedHelperTools/org.clash-qt.service ]; then
    echo "install: a privileged helper is installed and was NOT changed."
    echo "         If this version expects a different helper, open"
    echo "         Settings -> Privileged Service and choose Repair."
fi
