#!/usr/bin/env bash
# Virtual touch/tablet test (R5): inject touch through the SDE test-only
# serenity-virtual-input protocol and assert the compositor routes it — the
# client receives touch-down/motion/up, and a drag started from a touch works
# (the touch-drag path, where the drag icon follows the finger).
#
# wlroots offers no virtual touch/tablet and the headless backend has no
# libinput, so this is how touch/pen are exercised headlessly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
TOOLS_BUILD="$SDE_ROOT/build/virtual-touch-test"

command -v wayland-scanner >/dev/null || { echo "SKIP: no wayland-scanner"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh" 2>/dev/null | tail -1)"

mkdir -p "$TOOLS_BUILD"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/dnd-test.c" "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/dnd-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-vtouch.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
SOCK="sde-vtouch-$$"
COMP_PID=""
CLIENT_PID=""
cleanup() {
    [[ -n "$CLIENT_PID" ]] && kill "$CLIENT_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR"/$SOCK* 2>/dev/null || true
    rm -rf "$RUNTIME"
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

# Creating the virtual touch makes the seat advertise touch; wait for the client
# to bind wl_touch before injecting.
echo "=== inject: pen, then touch drag ==="
# Tablet first: the DnD client exits once its (touch) drag is cancelled, so the
# pen phase needs the window alive. The touch drag then runs last.
env WAYLAND_DISPLAY="$SOCK" "$VI" \
    --sleep 500 \
    --tablet-proximity 120 120 \
    --tablet-tip-down \
    --tablet-motion 240 180 \
    --tablet-axis 0.5 0 0 0 0 0 0 \
    --tablet-tip-up \
    --tablet-proximity-out \
    --sleep 200 \
    --touch 100 100 \
    --sleep 250 \
    --touch-motion 220 170 \
    --sleep 250 \
    --touch-up \
    --linger 400 >/dev/null 2>&1 || true
sleep 0.4

kill "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""

fail=0
check_client() {
    if grep -qE "$1" "$RUNTIME/client.log"; then
        echo "  ok: $2"
    else
        echo "  FAIL: $2"
        fail=1
    fi
}
check_comp() {
    if grep -qE "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $2"
    else
        echo "  FAIL: $2"
        fail=1
    fi
}

# The client starts a drag from the first touch-down, so the drag grab consumes
# the following motion/up (that is the touch-drag path). Assert the down reached
# the client and the drag ran to completion.
check_client "^touch-down"   "the client received touch-down"
check_comp "WindowServer: drag started" "a touch drag started"
check_comp "WindowServer: drag ended"   "the touch drag ended"
check_client "^drag-(cancelled|drop|finished)$" "the drag operation resolved"
check_comp "tablet tool in proximity" "the virtual pen reached the tablet path"
check_comp "tablet tool out of proximity" "the virtual pen left proximity"

if grep -qiE "SIGSEGV|Segmentation fault|VERIFICATION FAILED|ASSERTION FAILED" "$RUNTIME/comp.log" "$RUNTIME/client.log"; then
    echo "  FAIL: a process faulted"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- client log ---"; cat "$RUNTIME/client.log"
    echo "--- compositor log (drag/touch) ---"; grep -iE "drag|touch" "$RUNTIME/comp.log" | tail -20
    echo "==> virtual touch test FAILED"
    exit 1
fi
echo "==> virtual touch test PASSED"
