#!/usr/bin/env bash
# ci-tier: full
# Drag-to-edge snapping test (WS2, docs/14).
#
# Dragging a window's titlebar to a screen edge arms a snap (a tile preview, then
# tiling on release). It should only arm when the pointer is *close* to the edge
# ([Tiling] EdgeThreshold, default 16px), and [Tiling] DragToEdge=false must
# disable it entirely (default on).
#
# Headless: a native test client provides a titlebar to drag, driven through the
# wlroots virtual-pointer protocol. The compositor logs "tiled wm_id=N as T".
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true
NATIVE="$SDE_ROOT/build/tools/native-app-test"
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

# Run one case: Super+left-drag the window (at 0,0) to cursor x and return the
# compositor log path. $1 name, $2 drag x, $3 extra WindowServer.ini lines.
run_case() {
    local name="$1" drag_x="$2" config="$3"
    local runtime sock comp_pid
    runtime="$(mktemp -d "${TMPDIR:-/tmp}/sde-snap.XXXXXX")"
    chmod 700 "$runtime"
    {
        echo "[Placement]"
        echo "Windows=SDE Native Test:0,0,400,300"
        [[ -n "$config" ]] && printf '%s\n' "$config"
    } > "$runtime/WindowServer.ini"

    sock="sde-snap-$$-$name"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_STATE_DIR="$runtime" SDE_WM_PORTAL_PATH="$runtime/wm" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$runtime/comp.log" 2>&1 &
    comp_pid=$!
    for _ in $(seq 1 50); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    env WAYLAND_DISPLAY="$sock" setsid timeout 8 "$NATIVE" > "$runtime/native.log" 2>&1 &
    sleep 1.5

    # Super+left-drag the window body (the test client is undecorated, so there is
    # no titlebar) to the right edge (x=drag_x; the headless output is 1280x720).
    # This is the same move path a titlebar drag uses, including edge snapping.
    env WAYLAND_DISPLAY="$sock" "$VI" \
        --extent 1280 720 \
        --move 200 150 --key 125 press --button 0x110 press --sleep 60 \
        --move "$drag_x" 300 --move "$drag_x" 301 --sleep 60 \
        --button 0x110 release --sleep 60 --key 125 release --linger 200 >/dev/null 2>&1 || true
    sleep 0.6

    kill -TERM -- "-$comp_pid" 2>/dev/null || kill "$comp_pid" 2>/dev/null || true
    wait "$comp_pid" 2>/dev/null || true
    echo "$runtime/comp.log"
}

# Default threshold (16px): 5px from the edge snaps, 40px does not.
EDGE_LOG="$(run_case edge 1275 "")"
AWAY_LOG="$(run_case away 1240 "")"
# DragToEdge=false: at the very edge, still no snap.
OFF_LOG="$(run_case off 1275 $'[Tiling]\nDragToEdge=false')"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== config ==="
grep -h "tiling edge" "$EDGE_LOG" "$OFF_LOG" 2>/dev/null || true

check "at the edge: the window snaps (tiled right)" "grep -q 'tiled wm_id=1 as 2' \"$EDGE_LOG\""
check "40px from the edge: no snap" "! grep -q 'tiled wm_id=' \"$AWAY_LOG\""
check "DragToEdge=false: no snap even at the edge" "! grep -q 'tiled wm_id=' \"$OFF_LOG\""

if [[ "$fail" -ne 0 ]]; then
    echo "--- edge log (tail) ---"; tail -12 "$EDGE_LOG"
    echo "--- away log (tail) ---"; tail -12 "$AWAY_LOG"
    echo "--- off log (tail) ---"; tail -12 "$OFF_LOG"
    echo "==> edge-snap test FAILED"
    exit 1
fi
echo "==> edge-snap test PASSED"
