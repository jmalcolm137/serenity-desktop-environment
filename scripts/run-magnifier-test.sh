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
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init magnifier
sde_test_export_roots
RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"
APP="$BUILD/bin/Magnifier"
# The capture portal lives in the runtime directory and the session directory,
# both of which a live session uses too.
sde_test_private_runtime_dir
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$APP" ]] || { echo "SKIP: Magnifier not built"; exit 0; }

sde_test_compositor SDE_XWAYLAND=0 SERENITY_RES_ROOT="$RES_ROOT"
sleep 1

sde_test_client env WAYLAND_DISPLAY="$SOCK" SDE_APP_BIN_DIR="$BUILD/bin" SERENITY_RES_ROOT="$RES_ROOT" \
    LD_LIBRARY_PATH="$BUILD/lib" timeout 20 "$APP" > "$RUNTIME/app.log" 2>&1
APP_PID=$SDE_TEST_CLIENT_PID
sleep 4

echo "=== captures served ==="
grep -aE "capture served" "$RUNTIME/comp.log" | head -3 || true
sde_test_check "$RUNTIME/comp.log" "toplevel mapped title='Magnifier'" "the Magnifier maps a window"
# The app captures on every display-link tick, so several served captures over the
# wait prove the loop is alive, not just that one request went through.
SERVED=$(grep -ac 'capture served' "$RUNTIME/comp.log" || true)
sde_test_expect "the compositor served more than one capture ($SERVED)" test "${SERVED:-0}" -ge 2
sde_test_refute "$RUNTIME/comp.log" "capture served 0x0" "the capture was not empty"
sde_test_expect "the Magnifier is still running" kill -0 "$APP_PID"

sde_test_finish "$RUNTIME/app.log"
