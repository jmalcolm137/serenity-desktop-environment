#!/usr/bin/env bash
# Terminal mouse-reporting test (R5): a full-screen TUI (like opencode) enables
# DECSET 1000/1002/1003 + SGR (1006) and expects the terminal to forward wheel
# and pointer events. LibVT used to implement none of it, so such apps could
# neither scroll themselves nor use the scrollback (which the alternate screen
# disables).
#
# Runs headless: it starts the SDE compositor, runs Terminal with a shell that
# enables mouse reporting and captures stdin, injects a wheel event through the
# wlroots virtual-pointer protocol, and asserts the SGR wheel sequence arrived.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init terminal-mouse
sde_test_export_roots
TERMINAL="$BUILD/bin/Terminal"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build.sh" Terminal >/dev/null
VI="$(sde_test_vi)"

# A mouse-reporting "app": enable press/release + any-motion + SGR, then capture
# everything the terminal sends on stdin. (The Terminal only forwards a few env
# vars to its child, so bake the output path in.)
cat > "$RUNTIME/mouse-echo.sh" <<EOF
#!/bin/sh
stty raw -echo
printf '\\033[?1000h\\033[?1002h\\033[?1006h'
exec cat > "$RUNTIME/mouse-out.bin"
EOF
chmod +x "$RUNTIME/mouse-echo.sh"

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"
sleep 0.4

MOUSE_OUT="$RUNTIME/mouse-out.bin"
sde_test_client env WAYLAND_DISPLAY="$SOCK" SHELL="$RUNTIME/mouse-echo.sh" \
    "$TERMINAL" -e "$RUNTIME/mouse-echo.sh" > "$RUNTIME/terminal.log" 2>&1
TERM_PID=$SDE_TEST_CLIENT_PID
sleep 3
if ! kill -0 "$TERM_PID" 2>/dev/null; then
    echo "FAIL: Terminal exited early ($(head -1 "$RUNTIME/terminal.log"))"
    exit 1
fi

# Move into the terminal, then scroll down (positive vertical axis).
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --move 300 300 >/dev/null 2>&1 || true
sleep 0.5
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --axis 0 1 --axis 0 1 >/dev/null 2>&1 || true
sleep 1
sde_test_kill "$TERM_PID"

echo "=== bytes received by the app (od) ==="
od -An -tx1 "$MOUSE_OUT" 2>/dev/null | head || true

echo "=== terminal mouse reporting ==="
sde_test_check "$MOUSE_OUT" $'\033\\[<6[45];' "the terminal forwarded an SGR wheel event to the app"
sde_test_finish
