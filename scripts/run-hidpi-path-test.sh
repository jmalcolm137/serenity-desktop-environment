#!/usr/bin/env bash
# Regression test: a filled Gfx::Path must render at >1x (HiDPI), not crash.
#
# EdgeFlagPathRasterizer used to VERIFY(painter.scale() == 1), which aborted
# SystemMonitor's Performance tab (GraphWidget fills the area under the graph) on
# a 2x output. We render that tab at 2x and require a non-empty backing store
# with no verification failure (port patch 0027).
#
# Screenshot/backing-store based (D7). Runs headless, so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
APP="$BUILD/bin/SystemMonitor"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
if [[ ! -x "$APP" ]]; then
    echo "==> SystemMonitor not built at $APP"
    exit 1
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-hidpi-path.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-hidpi-path-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    SDE_OUTPUT_SCALE=2 \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    LIBWM_DUMP="$RUNTIME/2x.png" \
    "$COMP" -n "$SOCK" -s "$APP -t graphs" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 80); do
    [[ -s "$RUNTIME/2x.png" ]] && break
    sleep 0.1
done
sleep 1

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

fail=0
echo "=== HiDPI filled-path test (2x) ==="
if [[ -s "$RUNTIME/2x.png" ]]; then
    echo "  ok: 2x backing store rendered ($(stat -c%s "$RUNTIME/2x.png") bytes)"
else
    echo "  MISSING: 2x backing store dump"
    fail=1
fi

if grep -aq "VERIFICATION FAILED" "$RUNTIME/comp.log"; then
    echo "  FAIL: verification failed while filling a path at 2x:"
    grep -a "VERIFICATION FAILED" "$RUNTIME/comp.log" | head -2
    fail=1
else
    echo "  ok: no verification failure"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> HiDPI filled-path test FAILED"
    exit 1
fi
echo "==> HiDPI filled-path test PASSED"
