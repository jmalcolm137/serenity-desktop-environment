#!/usr/bin/env bash
# ci-tier: fast
# Screen magnifier (R5 accessibility, docs/29-accessibility.md).
#
# The Magnifier captures the screen through
# GUI::ConnectionToWindowServer::the().get_screen_bitmap_around_cursor() /
# …_around_location(). On the port those used to resolve to the default stub in
# WindowServerDefaultStub.h, which returns a null bitmap -- so the window mapped and
# painted nothing, with no error anywhere. The compositor now renders the requested
# region offscreen and pushes it back over serenity-window (request_capture ->
# captured); the shim caches it and answers from that.
#
# Asserts three things: the window maps, captures are served at the size the app
# asks for (so a blank magnifier fails), and the app stays up while it does.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
APP="$BUILD/bin/Magnifier"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$APP" ]] || { echo "SKIP: Magnifier not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-magnifier.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/run" "$RUNTIME/session/portal"
SOCK="sde-magnifier-$$"
APP_PID=""
COMP_PID=""
cleanup() {
    [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM "$COMP_PID" 2>/dev/null || true
    # Give them a moment to exit, or the tree is still being written while it is
    # removed ("Directory not empty").
    sleep 0.3
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 SDE_XWAYLAND=0 \
    XDG_RUNTIME_DIR="$RUNTIME/run" SDE_STATE_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" \
    SERENITY_RES_ROOT="$RES_ROOT" LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "$RUNTIME/run/$SOCK" ]] && break
    sleep 0.1
done
sleep 1

env WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME/run" SDE_STATE_DIR="$RUNTIME" \
    SDE_SESSION_DIR="$RUNTIME/session" SDE_APP_BIN_DIR="$BUILD/bin" \
    SERENITY_RES_ROOT="$RES_ROOT" LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 20 "$APP" > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!
sleep 4

ALIVE="no"
kill -0 "$APP_PID" 2>/dev/null && ALIVE="yes"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== captures served ==="
grep -aE "capture served" "$RUNTIME/comp.log" | head -3 || true
check "the Magnifier maps a window" "grep -q \"toplevel mapped title='Magnifier'\" '$RUNTIME/comp.log'"
# The app captures on every display-link tick, so several served captures over the
# wait prove the loop is alive, not just that one request went through.
SERVED=$(grep -ac 'capture served' "$RUNTIME/comp.log" || true)
check "the compositor served more than one capture" "[[ '${SERVED:-0}' -ge 2 ]]"
check "the capture was not empty" "! grep -q 'capture served 0x0' '$RUNTIME/comp.log'"
check "the Magnifier is still running" "[[ '$ALIVE' == yes ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -20 "$RUNTIME/comp.log"
    echo "--- magnifier log (tail) ---"; tail -10 "$RUNTIME/app.log"
    echo "==> magnifier test FAILED"
    exit 1
fi
echo "==> magnifier test PASSED"
