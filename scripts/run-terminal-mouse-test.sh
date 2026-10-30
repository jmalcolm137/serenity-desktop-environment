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
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TERMINAL="$BUILD/bin/Terminal"
TOOLS_BUILD="$SDE_ROOT/build/terminal-mouse-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build.sh" Terminal >/dev/null

mkdir -p "$TOOLS_BUILD"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-tmouse.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

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

SOCK="sde-tmouse-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
trap 'kill "$COMP_PID" 2>/dev/null || true; for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.4

MOUSE_OUT="$RUNTIME/mouse-out.bin"
env WAYLAND_DISPLAY="$SOCK" SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" SHELL="$RUNTIME/mouse-echo.sh" \
    "$TERMINAL" -e "$RUNTIME/mouse-echo.sh" > "$RUNTIME/terminal.log" 2>&1 &
TERM_PID=$!
sleep 3
if ! kill -0 "$TERM_PID" 2>/dev/null; then
    echo "FAIL: Terminal exited early ($(head -1 "$RUNTIME/terminal.log"))"
    exit 1
fi

# Move into the terminal, then scroll down (positive vertical axis).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" --extent 1280 720 --move 300 300 >/dev/null 2>&1 || true
sleep 0.5
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" --extent 1280 720 --axis 0 1 --axis 0 1 >/dev/null 2>&1 || true
sleep 1

kill "$TERM_PID" 2>/dev/null || true
wait "$TERM_PID" 2>/dev/null || true

echo "=== bytes received by the app (od) ==="
od -An -tx1 "$MOUSE_OUT" 2>/dev/null | head || true

fail=0
if [[ -s "$MOUSE_OUT" ]] && grep -aqE $'\033\[<6[45];' "$MOUSE_OUT"; then
    echo "  ok: the terminal forwarded an SGR wheel event to the app"
else
    echo "  FAIL: no SGR mouse event reached the app"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> terminal mouse test FAILED"
    exit 1
fi
echo "==> terminal mouse test PASSED"
