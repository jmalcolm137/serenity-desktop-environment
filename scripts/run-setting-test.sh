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
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init setting
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tests.sh" libwm-setting-test >/dev/null 2>&1 || true
TEST="$SDE_ROOT/build/test/libwm-setting-test"
[[ -x "$TEST" ]] || { echo "SKIP: libwm-setting-test not built"; exit 0; }

chmod 700 "$RUNTIME"

sde_test_compositor SDE_STATE_DIR="$RUNTIME"

# Keep the payload's status: an application that dies after applying the setting
# leaves an empty log (stdout to a file is fully buffered, so a crash loses it) and
# the assertions below would blame the shim instead of the crash.
APP_STATUS=0
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$TEST" > "$RUNTIME/app.log" 2>&1 || APP_STATUS=$?
sleep 0.5
kill -TERM -- "-$COMP_PID" 2>/dev/null || kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== setting round-trip ==="
grep -E "SETTING-TEST|host setting" "$RUNTIME/app.log" "$RUNTIME/comp.log" 2>/dev/null | head || true

check "the payload exited cleanly" "[[ '$APP_STATUS' -eq 0 ]]"
check "the shim answered the new value" "grep -q 'SETTING-TEST: accel .* -> 2.5' \"$RUNTIME/app.log\""
check "the shim answered the new double-click speed" "grep -q 'SETTING-TEST: double-click .* -> 321' \"$RUNTIME/app.log\""
check "the compositor received the setting" "grep -q 'host setting mouse.acceleration=2.5' \"$RUNTIME/comp.log\""
check "the compositor persisted it in input.ini" "grep -q '^Acceleration=2.5' \"$RUNTIME/input.ini\""

if [[ "$fail" -ne 0 ]]; then
    echo "--- app log (payload exit $APP_STATUS) ---"; tail -10 "$RUNTIME/app.log"
    echo "--- compositor log (tail) ---"; tail -15 "$RUNTIME/comp.log"
    echo "--- input.ini ---"; cat "$RUNTIME/input.ini" 2>/dev/null
    echo "==> setting test FAILED"
    exit 1
fi
echo "==> setting test PASSED"
