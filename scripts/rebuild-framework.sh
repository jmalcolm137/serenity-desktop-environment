#!/usr/bin/env bash
# Rebuild the framework (LibGUI/LibGfx/LibVT/LibWM) and the applications after a
# port change.
#
# Needed whenever a change lands in libgui-wayland's patches/ or src/: the ports'
# sources are patched into the shared SerenityOS checkout, and nothing takes effect
# until the framework and the affected applications are rebuilt.
#
# **Run this with no session running** — from a VT (Ctrl+Alt+F3), or before logging
# in. Relinking liblagom-vt or libwindowserver rewrites files that every running
# client has mapped, and a process that faults on a rewritten mapping dies with
# SIGBUS. The check below refuses rather than risk it; SDE_ALLOW_LIVE_REBUILD=1
# overrides at your own risk.
#
#   scripts/rebuild-framework.sh              # framework + Terminal + the applets
#   scripts/rebuild-framework.sh --with-apps  # plus the applications
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"

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

if live_session_running && [[ "${SDE_ALLOW_LIVE_REBUILD:-0}" != 1 ]]; then
    echo "refusing to rebuild: a session is running, and relinking the libraries it" >&2
    echo "  has mapped can kill its clients with SIGBUS." >&2
    echo "  Log out (or switch to a VT) and run this again." >&2
    echo "  To override, at your own risk: SDE_ALLOW_LIVE_REBUILD=1 $0" >&2
    exit 3
fi

echo "==> the framework (LibGfx/LibGUI/LibVT/LibWM)"
"$LIBGUI_WAYLAND_ROOT/scripts/build-libgui.sh" --no-tests \
    $("$SDE_ROOT/scripts/lagom-libraries.sh")

echo "==> the applications"
"$SDE_ROOT/scripts/build.sh" Terminal
"$SDE_ROOT/scripts/build.sh" FileManager
if [[ "${1:-}" == "--with-apps" ]]; then
    "$SDE_ROOT/scripts/build-full-sde.sh" --with-apps
fi

echo "==> done. Log back in, then check a key nobody else can check for you:"
echo "      od -An -c        # press Backspace, then Ctrl-C, then Ctrl-D"
echo "    Backspace should be 177 (\\b instead means the old build); Ctrl-C should be 003."
