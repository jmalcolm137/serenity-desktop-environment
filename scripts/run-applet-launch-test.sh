#!/usr/bin/env bash
# Headless test: clicking the ResourceGraph taskbar applet opens SystemMonitor.
#
# Covers two regressions:
#   * Process::resolve_executable_path() must consult SDE_APP_BIN_DIR: the applet
#     lives in build/Applets, so "/bin/SystemMonitor" must resolve to the session's
#     SystemMonitor (in the port's bin dir), not to <applet-dir>/SystemMonitor
#     (libgui-wayland patch 0011);
#   * no "Failed to parse IPC message" faults (the WindowServer IPC MessageID is
#     positional, so a stale applet/Taskbar would fail here).
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init applet-launch
sde_test_export_roots
APPLET="$SDE_ROOT/build/Applets/ResourceGraph"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh" 2>/dev/null | tail -1)"
SOCK="sde-launch-$$"
WIDTH=1280
HEIGHT=720

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-applets.sh" >/dev/null
[[ -x "$APPLET" ]] || { echo "SKIP: $APPLET not built"; exit 0; }
[[ -x "$BUILD/bin/SystemMonitor" ]] || { echo "SKIP: $BUILD/bin/SystemMonitor not built"; exit 0; }

export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
# Kill the compositor's whole process group: it spawns the Taskbar/applets via -s,
# and killing only the compositor would orphan them.
stop_compositor() {
    [[ -n "$COMP_PID" ]] || return 0
    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    kill -TERM "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
}

# The Taskbar and the ResourceGraph applet; the applet's click handler spawns
# /bin/SystemMonitor, resolved via SDE_APP_BIN_DIR (the port's bin dir).
STARTUP="$BUILD/bin/Taskbar & $APPLET --cpu=CPUGraph,#00bb00 --memory=MemoryGraph,#00bbbb --network=NetworkGraph,#bbbb00"

# setsid makes it a session/group leader, so the trap can kill the whole group.
sde_test_compositor --startup "$STARTUP" SDE_WM_PORTAL_PATH="$RUNTIME/wm" SDE_APP_BIN_DIR="$BUILD/bin"

# The applet strip sits at the right of the taskbar. Click across it (the exact
# placement depends on the theme/font, so sweep a few positions); clicking empty
# taskbar is harmless.
for x in 1095 1130 1165 1200; do
    env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
        --move "$x" $((HEIGHT - 12)) --button 0x110 press --sleep 50 --button 0x110 release --sleep 400 >/dev/null 2>&1 || true
done
sleep 3

stop_compositor

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi
    return 0
}

echo "=== clicking the ResourceGraph applet opens SystemMonitor ==="
check "toplevel mapped title='System Monitor'"

if grep -q "Failed to parse IPC message" "$RUNTIME/comp.log"; then
    echo "  FAIL: an app reported 'Failed to parse IPC message' (stale binary?)"
    fail=1
else
    echo "  ok: no IPC parse errors"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"; cat "$RUNTIME/comp.log"
    echo "==> applet launch test FAILED"
    exit 1
fi
echo "==> applet launch test PASSED"
