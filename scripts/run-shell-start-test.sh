#!/usr/bin/env bash
# Headless test: the shell starts on a real login.
#
# The compositor must be listening on the WindowManagerServer socket *before* it
# spawns the -s startup command: the Taskbar, WorkspacePicker and Keymap connect to
# it at startup and abort (VERIFICATION FAILED in ConnectionToWindowManagerServer::
# the()) if the connect fails. Spawning them first raced on a real SDDM login, which
# left a session with applets but no Taskbar/desktop. This runs the session's real
# startup list (Taskbar + the applets + the battery applet) and asserts it comes up
# cleanly.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init shell-start
sde_test_export_roots
SOCK="sde-shell-$$"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$BUILD/bin/Taskbar" ]] || { echo "SKIP: Taskbar not built"; exit 0; }

export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
stop_compositor() {
    [[ -n "$COMP_PID" ]] || return 0
    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    kill -TERM "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
}

# The real session startup: the shell (Taskbar + Desktop), the applets, and the
# battery applet. (FileManager -d is the SerenityOS Desktop - icons over the
# wallpaper.)
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"
APPLET_CMDS="$(sde_applet_startup_commands "$SDE_ROOT")"
BATTERY_APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"
STARTUP="$BUILD/bin/Taskbar & $BUILD/bin/FileManager -d"
[[ -n "$APPLET_CMDS" ]] && STARTUP="$STARTUP & $APPLET_CMDS"
[[ -x "$BATTERY_APPLET" ]] && STARTUP="$STARTUP & $BATTERY_APPLET"

sde_test_compositor --startup "$STARTUP" SDE_WM_PORTAL_PATH="$RUNTIME/portal/wm" SDE_APP_BIN_DIR="$BUILD/bin" SDE_SESSION_BIN_DIR="$RUNTIME/sbin"
# The Desktop (FileManager -d) can take a moment to come up, so wait for its
# layer surface rather than racing a fixed sleep.
for _ in $(seq 1 300); do
    grep -q "layer surface namespace='serenity-desktop'" "$RUNTIME/comp.log" && break
    sleep 0.1
done
# The compositor must have created the override path's parent directory and be
# listening there (the WM clients connect to this socket).
PORTAL_OK=0
[[ -S "$RUNTIME/portal/wm" ]] && PORTAL_OK=1
stop_compositor

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi
    return 0
}

echo "=== the shell starts with the WindowManagerServer already listening ==="
if [[ "${PORTAL_OK:-0}" -eq 1 ]]; then
    echo "  ok: compositor created and listens on $RUNTIME/portal/wm"
else
    echo "  FAIL: no WM portal socket at $RUNTIME/portal/wm"; fail=1
fi
check "title='Taskbar'"
check "layer surface namespace='serenity-taskbar'"
check "layer surface namespace='serenity-desktop'"
check "AppletManager: applet area size"

if grep -q "VERIFICATION FAILED" "$RUNTIME/comp.log"; then
    echo "  FAIL: a shell process aborted (VERIFICATION FAILED) - startup race?"
    grep -B2 -A4 "VERIFICATION FAILED" "$RUNTIME/comp.log" | head -20
    fail=1
else
    echo "  ok: no VERIFICATION FAILED"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -30 "$RUNTIME/comp.log"
    echo "==> shell start test FAILED"
    exit 1
fi
echo "==> shell start test PASSED"
