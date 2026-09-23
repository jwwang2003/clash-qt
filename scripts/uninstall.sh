#!/bin/sh
# Show what clash-qt has put on this machine, and remove the parts you choose.
#
# An installed clash-qt is not one thing. It is an application bundle you own, a
# data directory holding your profiles and backups, preference files, and -- if
# you ever enabled TUN -- a root-owned helper with its own copy of the engine and
# a launch daemon. Dragging the application to the Trash removes the first of
# those and leaves the rest, including the root-owned parts, running.
#
# Run with no arguments to see what is present. Nothing is removed unless you
# ask for it by name.
#
# Usage:
#   scripts/uninstall.sh                 show what is installed, change nothing
#   scripts/uninstall.sh --app           remove application bundles
#   scripts/uninstall.sh --data          remove profiles, backups and preferences
#   scripts/uninstall.sh --service       remove the privileged helper (needs sudo)
#   scripts/uninstall.sh --all           all three
#   scripts/uninstall.sh --app --yes     skip the confirmation prompt
#   scripts/uninstall.sh --dir DIR       also look in DIR (for a custom APPDIR)
#
# --data is not recoverable. It holds your imported profiles, your enhancement
# scripts and your local backups, and backups contain subscription credentials.

set -eu

do_app=0; do_data=0; do_service=0; assume_yes=0; any=0
extra_dir=""

while [ $# -gt 0 ]; do
    case "$1" in
        --app) do_app=1; any=1; shift ;;
        --data) do_data=1; any=1; shift ;;
        --service) do_service=1; any=1; shift ;;
        --all) do_app=1; do_data=1; do_service=1; any=1; shift ;;
        --yes|-y) assume_yes=1; shift ;;
        --dir) extra_dir="${2:?--dir needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "uninstall: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

HELPER_DIR="/Library/PrivilegedHelperTools/org.clash-qt.service"
DAEMON="/Library/LaunchDaemons/org.clash-qt.service.plist"
DATA="$HOME/Library/Application Support/clash-qt"

# Spotlight is how a stray copy actually reaches the user -- it is what offers
# three identical entries in search and in Open With -- so it is what we ask.
# The fallback matters on a machine where indexing is off or incomplete.
find_bundles() {
    { mdfind -name 'clash-qt.app' 2>/dev/null || true
      for d in /Applications "$HOME/Applications" $extra_dir; do
          [ -d "$d/clash-qt.app" ] && echo "$d/clash-qt.app"
      done
    } | sed '/^$/d' | sort -u
}

preference_files() {
    for f in "$HOME/Library/Preferences/com.clash-qt.clash-qt.plist" \
             "$HOME/Library/Preferences/org.clash-qt.desktop.plist" \
             "$HOME/Library/Preferences/clash-qt.plist"; do
        [ -f "$f" ] && echo "$f"
    done
    true
}

echo "clash-qt on this machine"
echo

echo "  Application bundles"
bundles="$(find_bundles)"
if [ -z "$bundles" ]; then
    echo "    none found"
else
    echo "$bundles" | while IFS= read -r b; do
        v="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
             "$b/Contents/Info.plist" 2>/dev/null || echo '?')"
        case "$b" in
            */build/*) note="  (a build artefact, not an installation)" ;;
            *) note="" ;;
        esac
        printf '    %-8s %s%s\n' "$v" "$b" "$note"
    done
fi

echo
echo "  Your data"
if [ -d "$DATA" ]; then
    echo "    $(du -sh "$DATA" 2>/dev/null | cut -f1)   $DATA"
else
    echo "    none"
fi
prefs="$(preference_files)"
if [ -n "$prefs" ]; then
    echo "$prefs" | sed 's|^|    preference   |'
fi

echo
echo "  Privileged helper (root-owned)"
if [ -d "$HELPER_DIR" ] || [ -f "$DAEMON" ]; then
    [ -d "$HELPER_DIR" ] && echo "    $HELPER_DIR"
    [ -f "$DAEMON" ] && echo "    $DAEMON"
    if launchctl print system/org.clash-qt.service >/dev/null 2>&1; then
        echo "    the launch daemon is currently LOADED"
    fi
else
    echo "    not installed"
fi

if [ "$any" -eq 0 ]; then
    echo
    echo "Nothing was changed. To remove something:"
    echo "  scripts/uninstall.sh --app      # application bundles"
    echo "  scripts/uninstall.sh --data     # profiles, backups, preferences (permanent)"
    echo "  scripts/uninstall.sh --service  # the privileged helper (asks for sudo)"
    echo "  scripts/uninstall.sh --all"
    echo
    echo "Installed somewhere else? Add --dir /that/directory." 
    exit 0
fi

echo
what=""
[ "$do_app" -eq 1 ] && what="$what application bundles,"
[ "$do_data" -eq 1 ] && what="$what your profiles/backups/preferences,"
[ "$do_service" -eq 1 ] && what="$what the privileged helper,"
what="$(echo "$what" | sed 's/,$//; s/^ //')"

if [ "$assume_yes" -eq 0 ]; then
    printf 'Remove %s? This cannot be undone. [y/N] ' "$what"
    read -r answer
    case "$answer" in y|Y|yes|YES) ;; *) echo "nothing removed"; exit 0 ;; esac
fi

# The helper goes first, and through its own uninstall rather than rm. That path
# does `launchctl bootout` before unlinking and refuses to remove a file that is
# not a root-owned regular file. Deleting the files directly would leave the
# daemon running against images that no longer exist.
if [ "$do_service" -eq 1 ]; then
    if [ -x "$HELPER_DIR/helper" ]; then
        echo "removing the privileged helper (sudo required)"
        sudo "$HELPER_DIR/helper" --uninstall
        echo "  helper removed"
    elif [ -d "$HELPER_DIR" ] || [ -f "$DAEMON" ]; then
        echo "  the helper binary is missing but its files remain;" >&2
        echo "  stopping the daemon and removing them directly" >&2
        sudo launchctl bootout system/org.clash-qt.service 2>/dev/null || true
        sudo rm -f "$DAEMON"
        sudo rm -rf "$HELPER_DIR"
        echo "  helper files removed"
    else
        echo "  no privileged helper installed"
    fi
fi

if [ "$do_app" -eq 1 ]; then
    if [ -z "$bundles" ]; then
        echo "  no application bundles found"
    else
        echo "$bundles" | while IFS= read -r b; do
            case "$b" in
                */build/*)
                    echo "  skipping $b (build artefact -- use 'make clean')" ;;
                *)
                    pkill -f "$b/Contents/MacOS/clash-qt" 2>/dev/null || true
                    if rm -rf "$b" 2>/dev/null; then echo "  removed $b"
                    else sudo rm -rf "$b" && echo "  removed $b (sudo)"; fi ;;
            esac
        done
    fi
fi

if [ "$do_data" -eq 1 ]; then
    [ -d "$DATA" ] && rm -rf "$DATA" && echo "  removed $DATA"
    preference_files | while IFS= read -r f; do rm -f "$f" && echo "  removed $f"; done
    # Preferences are cached by the system daemon; without this the values come
    # back on the next launch from a process that never read the disk.
    defaults delete com.clash-qt.clash-qt >/dev/null 2>&1 || true
    defaults delete org.clash-qt.desktop >/dev/null 2>&1 || true
    killall -u "$USER" cfprefsd >/dev/null 2>&1 || true
    echo "  preference cache flushed"
fi

echo
echo "done"
