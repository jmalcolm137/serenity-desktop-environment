#!/usr/bin/env bash
# Rebuild the framework (LibGUI/LibGfx/LibVT/LibWM) and the applications after a
# port change.
#
# Needed whenever a change lands in libgui-wayland's patches/ or src/: the ports'
# sources are patched into the shared SerenityOS checkout, and nothing takes effect
# until the framework and the affected applications are rebuilt.
#
# The SDE's own programs (the session services, the applets and the portals) link
# the same framework and compile against the same headers, so they are rebuilt
# here too. Header-level ABI is real: when LibIPC's ConnectionBase gained a
# member, a service built before the change read m_deferred_invoker at the old
# offset, crashed on the first disconnect, and took the Taskbar down with it
# (the Taskbar exits when its LaunchServer connection dies). Rebuilding only the
# framework leaves that mix in place until the next login.
#
# **Safe while you are logged in.** Measured rather than assumed: relinking a
# library *replaces* the file (new inode) instead of rewriting it, so a process that
# already has it mapped keeps the old mapping and carries on — verified with a live
# process reading an 8 MB library across a relink. Running clients therefore keep
# their old copy, and only *newly started* ones pick up the new code. After a rebuild
# for a client-side fix, open a new Terminal (a logout is not needed; a reboot
# certainly is not).
#
#   scripts/rebuild-framework.sh              # framework + Terminal + FileManager
#   scripts/rebuild-framework.sh --with-apps  # plus the applications
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

live_session_running() {
    local runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    [[ -S "$runtime/sde" ]] && return 0
    local heartbeat now modified
    now="$(date +%s)"
    for heartbeat in "$runtime"/sde-heartbeat*; do
        [[ -e "$heartbeat" ]] || continue
        modified="$(stat -c %Y "$heartbeat" 2>/dev/null || echo 0)"
        (( now - modified < 60 )) && return 0
    done
    return 1
}

if live_session_running; then
    echo "note: a session is running. That is fine — relinking replaces the library" >&2
    echo "      files, so running clients keep the old ones and nothing is disturbed." >&2
    echo "      Open a new Terminal afterwards to pick up a client-side fix." >&2
fi

echo "==> the framework (LibGfx/LibGUI/LibVT/LibWM)"
"$LIBGUI_WAYLAND_ROOT/scripts/build-libgui.sh" --no-tests \
    $("$SDE_ROOT/scripts/lagom-libraries.sh")

echo "==> the applications"
"$SDE_ROOT/scripts/build.sh" Terminal
"$SDE_ROOT/scripts/build.sh" FileManager
# The application suite links the same framework. Rebuild it when this tree has it
# (a tree built without apps skips this); a framework header change leaves every
# app built before it reading the old layout - DisplaySettings crashed on its
# first get_system_theme() after the accessibility rebuild.
if [[ -x "$BUILD/bin/DisplaySettings" ]]; then
    echo "==> the application suite (it is built in this tree)"
    "$SDE_ROOT/scripts/build-apps.sh"
fi

# The SDE's own framework consumers. This is the incremental top-level build:
# CMake knows which of them include a header that changed, so an up-to-date tree
# is a no-op and a header change rebuilds exactly the stale binaries.
echo "==> the SDE services, applets and portals"
"$SDE_ROOT/scripts/build-target.sh"

if [[ "${1:-}" == "--with-apps" ]]; then
    "$SDE_ROOT/scripts/build-full-sde.sh" --with-apps
fi

echo "==> done. Open a *new* Terminal (an existing one keeps the libraries it already"
echo "    mapped, so it will behave exactly as before), then check:"
echo "      od -An -c        # press Backspace, then Ctrl-C, then Ctrl-D"
echo "    Backspace should be 177 (\\b means an old build); Ctrl-C should be 003."
