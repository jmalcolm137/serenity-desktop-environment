#!/usr/bin/env bash
# Setting round-trip test.
#
# Changes a setting the desktop owns (the mouse acceleration) through the
# app-facing WindowServer protocol and reads it back. The LibWindowServer shim
# must hand the change to the compositor over serenity-window
# (set_host_setting) and answer from what it is told; the compositor applies and
# persists it in its own configuration (input.ini) — the shim must not write it.
#
# See docs/24-libgui-wayland-sde-boundary.md.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tests.sh" libwm-setting-test >/dev/null 2>&1 || true
TEST="$SDE_ROOT/build/test/libwm-setting-test"
[[ -x "$TEST" ]] || { echo "SKIP: libwm-setting-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-setting.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-setting-$$"
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$TEST" > "$RUNTIME/app.log" 2>&1 || true
sleep 0.5
kill -TERM -- "-$COMP_PID" 2>/dev/null || kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== setting round-trip ==="
grep -E "SETTING-TEST|host setting" "$RUNTIME/app.log" "$RUNTIME/comp.log" 2>/dev/null | head || true

check "the shim answered the new value" "grep -q 'SETTING-TEST: accel .* -> 2.5' \"$RUNTIME/app.log\""
check "the shim answered the new double-click speed" "grep -q 'SETTING-TEST: double-click .* -> 321' \"$RUNTIME/app.log\""
check "the compositor received the setting" "grep -q 'host setting mouse.acceleration=2.5' \"$RUNTIME/comp.log\""
check "the compositor persisted it in input.ini" "grep -q '^Acceleration=2.5' \"$RUNTIME/input.ini\""

if [[ "$fail" -ne 0 ]]; then
    echo "--- app log ---"; tail -10 "$RUNTIME/app.log"
    echo "--- compositor log (tail) ---"; tail -15 "$RUNTIME/comp.log"
    echo "--- input.ini ---"; cat "$RUNTIME/input.ini" 2>/dev/null
    echo "==> setting test FAILED"
    exit 1
fi
echo "==> setting test PASSED"
