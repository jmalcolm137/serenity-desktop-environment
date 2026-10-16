#!/usr/bin/env bash
# Headless test for the SDE taskbar applets (the SerenityOS applets, built by
# SDE: scripts/build-applets.sh). It runs the real applets under the headless
# compositor and asserts each maps as its "serenity-applet:<name>" layer surface
# (so it lands in the Taskbar's applet area), and that none of them abort.
#
# The applets that talk to the WindowManagerServer (WorkspacePicker, Keymap) use
# the standard /tmp/portal/wm socket, so this test runs against that path. Run it
# when no live SDE session is using it.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
APPLETS="$SDE_ROOT/build/Applets"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-applets.sh" >/dev/null
"$SDE_ROOT/scripts/build-services.sh" >/dev/null 2>&1 || true

# The Audio applet needs the AudioServer service. Assemble the session app directory
# the way a session does (scripts/sde-session.sh), so LibWM finds the service by name
# wherever it was built, rather than relying on the framework's bin/.
APP_BIN="$(mktemp -d "${TMPDIR:-/tmp}/sde-applets-bin.XXXXXX")"
for service in AudioServer NotificationServer; do
    [[ -x "$SDE_ROOT/build/$service/$service" ]] && ln -sf "$SDE_ROOT/build/$service/$service" "$APP_BIN/$service"
done

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-applets.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
WM_PORTAL="${SDE_WM_PORTAL_PATH:-/tmp/portal/wm}"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME" "$APP_BIN"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock "$WM_PORTAL" 2>/dev/null || true' EXIT

SOCK="sde-applets-$$"
rm -f "$WM_PORTAL"

# The same set and arguments as the session startup (scripts/sde-applets.sh),
# which mirror SerenityOS's Base/etc/SystemServerUser.ini.
STARTUP="$APPLETS/WorkspacePicker"
STARTUP="$STARTUP & $APPLETS/ResourceGraph --cpu=CPUGraph,#00bb00 --memory=MemoryGraph,#00bbbb --network=NetworkGraph,#bbbb00"
STARTUP="$STARTUP & $APPLETS/Network --name=Network"
STARTUP="$STARTUP & $APPLETS/ClipboardHistory"
STARTUP="$STARTUP & $APPLETS/Keymap"
STARTUP="$STARTUP & $APPLETS/Audio"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" SDE_APP_BIN_DIR="$APP_BIN" \
    "$COMP" -n "$SOCK" -s "$STARTUP" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 60); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 4

kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

LOG="$RUNTIME/comp.log"
fail=0
check() {
    if grep -qF "$1" "$LOG"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}
check_absent() {
    if grep -qF "$1" "$LOG"; then
        echo "  UNEXPECTED: $1"
        fail=1
    else
        echo "  ok (absent): $1"
    fi
}

echo "=== SDE applets map into the applet area ==="
check "namespace='serenity-applet:WorkspacePicker'"
check "namespace='serenity-applet:Keymap'"
check "namespace='serenity-applet:CPUGraph'"
check "namespace='serenity-applet:MemoryGraph'"
check "namespace='serenity-applet:NetworkGraph'"
check "namespace='serenity-applet:Network'"
check "namespace='serenity-applet:ClipboardHistory'"
check "namespace='serenity-applet:Audio'"
check "'Keymap' 16x16"
check "'WorkspacePicker' 28x15"
check "'Audio' 16x16"

echo "=== no applet aborted, and the Linux data sources read cleanly ==="
check_absent "VERIFICATION FAILED"
check_absent "Runtime error"
check_absent "Couldn't update adapter info"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"
    cat "$LOG"
    echo "==> SDE applets test FAILED"
    exit 1
fi
echo "==> SDE applets test PASSED"
