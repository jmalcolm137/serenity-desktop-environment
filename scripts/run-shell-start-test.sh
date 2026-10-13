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
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SOCK="sde-shell-$$"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$BUILD/bin/Taskbar" ]] || { echo "SKIP: Taskbar not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-shell.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
COMP_PID=""
stop_compositor() {
    [[ -n "$COMP_PID" ]] || return 0
    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    kill -TERM "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""
}
trap 'stop_compositor; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

# The real session startup: the shell, the applets, and the battery applet.
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"
APPLET_CMDS="$(sde_applet_startup_commands "$SDE_ROOT")"
BATTERY_APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"
STARTUP="$BUILD/bin/Taskbar & $BUILD/bin/TextEditor"
[[ -n "$APPLET_CMDS" ]] && STARTUP="$STARTUP & $APPLET_CMDS"
[[ -x "$BATTERY_APPLET" ]] && STARTUP="$STARTUP & $BATTERY_APPLET"

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_APP_BIN_DIR="$BUILD/bin" \
    SDE_SESSION_BIN_DIR="$RUNTIME/sbin" \
    "$COMP" -n "$SOCK" -s "$STARTUP" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 4
stop_compositor

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi
    return 0
}

echo "=== the shell starts with the WindowManagerServer already listening ==="
check "title='Taskbar'"
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
