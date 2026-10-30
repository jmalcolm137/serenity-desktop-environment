#!/usr/bin/env bash
# Drag-and-drop test (R5): a client starts an explicit data-device DnD from its
# window; assert the compositor accepts it, renders the drag icon, routes it and
# ends it cleanly when the button is released.
#
# Runs headless: the compositor, the source client and the virtual pointer.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
TOOLS_BUILD="$SDE_ROOT/build/dnd-test"

command -v wayland-scanner >/dev/null || { echo "SKIP: no wayland-scanner"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh" 2>/dev/null | tail -1)"

mkdir -p "$TOOLS_BUILD"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/dnd-test.c" "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/dnd-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-dnd.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
SOCK="sde-dnd-$$"
COMP_PID=""
CLIENT_PID=""
cleanup() {
    [[ -n "$CLIENT_PID" ]] && kill "$CLIENT_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/$SOCK* 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_OUTPUT_SCALE=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.4

env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/dnd-test" > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 1 60); do
    grep -q '^mapped$' "$RUNTIME/client.log" 2>/dev/null && break
    sleep 0.1
done

# The window maps at the default position (0,0); move the pointer inside it.
echo "=== drive: move onto the window, press, drag, release ==="
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --move 100 100 >/dev/null 2>&1 || true
sleep 0.3
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --button 0x110 press >/dev/null 2>&1 || true
sleep 0.5
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --move 300 200 >/dev/null 2>&1 || true
sleep 0.3
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --button 0x110 release >/dev/null 2>&1 || true
sleep 0.6

kill "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""

fail=0
if grep -q "WindowServer: drag started (icon=yes)" "$RUNTIME/comp.log"; then
    echo "  ok: the compositor accepted the drag and set up its icon"
else
    echo "  FAIL: the compositor did not accept a drag"
    fail=1
fi
if grep -q "WindowServer: drag ended" "$RUNTIME/comp.log"; then
    echo "  ok: the drag ended cleanly"
else
    echo "  FAIL: the drag did not end"
    fail=1
fi
if grep -qE "^drag-(cancelled|drop|finished)$" "$RUNTIME/client.log"; then
    echo "  ok: the source observed the drag lifecycle end"
else
    echo "  FAIL: the source did not observe the drag end"
    fail=1
fi
if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$RUNTIME/comp.log"; then
    echo "  FAIL: the compositor faulted"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"; tail -30 "$RUNTIME/comp.log"
    echo "--- client log ---"; cat "$RUNTIME/client.log"
    echo "==> dnd test FAILED"
    exit 1
fi
echo "==> dnd test PASSED"
