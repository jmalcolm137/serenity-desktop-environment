#!/usr/bin/env bash
# ci-tier: full
# Drag-to-edge snapping test (WS2, docs/14).
#
# Dragging a window's titlebar to a screen edge arms a snap (a tile preview, then
# tiling on release). It should only arm when the pointer is *close* to the edge
# ([Tiling] EdgeThreshold, default 16px), and [Tiling] DragToEdge=false must
# disable it entirely (default on).
#
# Headless: a native test client provides a window to drag, driven through the
# wlroots virtual-pointer protocol. The compositor logs "tiled wm_id=N as T".
#
# Each case is its own compositor and configuration (scripts/lib/testlib.sh's
# sde_test_case), so no case can leak a setting into the next.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init edge-snap
sde_test_export_roots

sde_test_build WindowServer sde-tools
VI="$(sde_test_vi)"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

# Run one case: Super+left-drag the window (at 0,0) to cursor x, then leave its
# compositor log path in CASE_LOG. $1 name, $2 drag x, $3 extra WindowServer.ini.
run_case() {
    sde_test_case "$1"
    {
        echo "[Placement]"
        echo "Windows=SDE Native Test:0,0,400,300"
        [[ -n "$3" ]] && printf '%s\n' "$3"
    } > "$CASE_RUNTIME/WindowServer.ini"
    sde_test_case_compositor SDE_WM_PORTAL_PATH="$CASE_RUNTIME/wm"

    env WAYLAND_DISPLAY="$CASE_SOCK" setsid timeout 8 "$NATIVE" > "$CASE_RUNTIME/native.log" 2>&1 &
    sde_test_wait_log "$CASE_RUNTIME/comp.log" "toplevel mapped" 40 || true
    sleep 1

    # Super+left-drag the window body (the test client is undecorated, so there is
    # no titlebar) to the right edge (x=$2; the headless output is 1280x720). This
    # is the same move path a titlebar drag uses, including edge snapping.
    env WAYLAND_DISPLAY="$CASE_SOCK" "$VI" \
        --extent 1280 720 \
        --move 200 150 --key 125 press --button 0x110 press --sleep 60 \
        --move "$2" 300 --move "$2" 301 --sleep 60 \
        --button 0x110 release --sleep 60 --key 125 release --linger 200 >/dev/null 2>&1 || true
    sleep 0.6

    sde_test_case_stop
    CASE_LOG="$CASE_RUNTIME/comp.log"
}

# Default threshold (16px): 5px from the edge snaps, 40px does not.
run_case edge 1275 ""
EDGE_LOG="$CASE_LOG"
run_case away 1240 ""
AWAY_LOG="$CASE_LOG"
# DragToEdge=false: at the very edge, still no snap.
run_case off 1275 $'[Tiling]\nDragToEdge=false'
OFF_LOG="$CASE_LOG"

echo "=== config ==="
grep -h "tiling edge" "$EDGE_LOG" "$OFF_LOG" 2>/dev/null || true

sde_test_check "$EDGE_LOG" 'tiled wm_id=1 as 2' "at the edge: the window snaps (tiled right)"
sde_test_refute "$AWAY_LOG" 'tiled wm_id=' "40px from the edge: no snap"
sde_test_refute "$OFF_LOG" 'tiled wm_id=' "DragToEdge=false: no snap even at the edge"

sde_test_finish "$EDGE_LOG" "$AWAY_LOG" "$OFF_LOG"
