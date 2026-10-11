#!/usr/bin/env bash
# Headless smoke test for the Linux-native SystemMonitor (R4.9).
#
# SystemMonitor no longer reads SerenityOS /sys/kernel files: on Linux it reads
# /proc directly (process list, memory) and unwinds thread stacks with
# libunwind + elfutils. This test just proves it launches into the compositor,
# maps its window, and does not fault.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SYSTEM_MONITOR="$BUILD/bin/SystemMonitor"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

if [[ ! -x "$SYSTEM_MONITOR" ]]; then
    echo "==> SystemMonitor not built at $SYSTEM_MONITOR"
    echo "    build it with: ninja -C $BUILD SystemMonitor"
    exit 1
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-sysmon.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-sysmon-$$"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

env WAYLAND_DISPLAY="$SOCK" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 5 "$SYSTEM_MONITOR" > "$RUNTIME/sysmon.log" 2>&1 || true
sleep 0.5

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
grep -iE "toplevel mapped|System Monitor" "$RUNTIME/comp.log" || true
echo "--- SystemMonitor log ---"
tail -20 "$RUNTIME/sysmon.log"

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
    return 0
}

echo "=== SystemMonitor headless smoke test ==="
check "toplevel mapped title='System Monitor'" "$RUNTIME/comp.log"

if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$RUNTIME/sysmon.log"; then
    echo "  FAIL: SystemMonitor reported a fault"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> SystemMonitor smoke test FAILED"
    exit 1
fi
echo "==> SystemMonitor smoke test PASSED"
