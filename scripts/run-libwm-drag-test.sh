#!/usr/bin/env bash
# LibGUI drag-and-drop bridge test (R5): a Serenity LibGUI app starts a
# DragOperation on a mouse press; assert the shim bridges it onto the standard
# Wayland data-device (the compositor accepts and routes the drag) and that the
# DragOperation resolves when the drag ends.
#
# Runs headless: the compositor, the LibGUI source app and the virtual pointer.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

if [[ ! -x "$SDE_ROOT/build/test/libwm-drag-test" ]]; then
    echo "building libwm-drag-test..."
    "$SDE_ROOT/scripts/build-tests.sh" libwm-drag-test >/dev/null 2>&1 || true
fi
[[ -x "$SDE_ROOT/build/test/libwm-drag-test" ]] || { echo "SKIP: $SDE_ROOT/build/test/libwm-drag-test is missing"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh" 2>/dev/null | tail -1)"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-libwmdrag.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
SOCK="sde-libwmdrag-$$"
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

env WAYLAND_DISPLAY="$SOCK" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$SDE_ROOT/build/test/libwm-drag-test" > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 1 80); do
    grep -q "toplevel mapped.*LibWM Drag Test" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.3

echo "=== drive: press on the drag widget, drag, release ==="
# One virtual-input session for the whole gesture: separate invocations create
# and destroy the virtual pointer, so a button can arrive before the client has
# acquired the new pointer (the same warm-up race as the input test).
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
    --move 200 200 --sleep 200 \
    --button 0x110 press --sleep 500 \
    --move 260 190 --sleep 300 \
    --button 0x110 release --linger 800 >/dev/null 2>&1 || true
sleep 0.8

kill "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""

fail=0
if grep -q "WindowServer: drag started" "$RUNTIME/comp.log"; then
    echo "  ok: the compositor accepted the LibGUI drag"
else
    echo "  FAIL: the LibGUI drag never reached the compositor"
    fail=1
fi
if grep -q "WindowServer: drag ended" "$RUNTIME/comp.log"; then
    echo "  ok: the drag ended cleanly"
else
    echo "  FAIL: the drag did not end"
    fail=1
fi
if grep -qE "DRAG-TEST: outcome=(accepted|cancelled)" "$RUNTIME/client.log"; then
    echo "  ok: the DragOperation resolved"
else
    echo "  FAIL: the DragOperation did not resolve"
    fail=1
fi
if grep -qiE "SIGSEGV|Segmentation fault|VERIFICATION FAILED|ASSERTION FAILED" "$RUNTIME/comp.log" "$RUNTIME/client.log"; then
    echo "  FAIL: a process faulted"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (drag) ---"; grep -iE "drag|toplevel mapped" "$RUNTIME/comp.log" | tail -20
    echo "--- client log ---"; cat "$RUNTIME/client.log"
    echo "==> libwm drag test FAILED"
    exit 1
fi
echo "==> libwm drag test PASSED"
