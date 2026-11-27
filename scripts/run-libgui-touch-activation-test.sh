#!/usr/bin/env bash
# ci-tier: full
# LibWM touch and activation against the real SDE compositor.
#
# Touch: a LibWM test window receives a virtual touch and reports it as a
# primary-pointer press/release (LibGUI has no touch event type).
# Activation: a window opened from a click asks xdg-activation-v1 to be focused,
# and the SDE compositor switches to and focuses it.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init libgui-touch-activation
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
sde_test_build WindowServer sde-tools
ninja -C "$BUILD" libwm-test-window libwm-activation-test >/dev/null
VI="$(sde_test_vi)"

sde_test_compositor LIBWM_WM_PORTAL_PATH="$RUNTIME/wm"

placement() {
    sed -n "s/.*placed new window '$1' at \\([-0-9]*\\),\\([-0-9]*\\).*/\\1 \\2/p" "$RUNTIME/comp.log" | tail -1
}

# --- touch ------------------------------------------------------------------
env WAYLAND_DISPLAY="$SOCK" LIBWM_TRACE=1 \
    "$BUILD/bin/libwm-test-window" > "$RUNTIME/touch.log" 2>&1 &
TOUCH_PID=$!
SDE_TEST_CLIENT_PIDS+=("$TOUCH_PID")
sde_test_wait_log "$RUNTIME/comp.log" "toplevel mapped title='LibWM Test Window'" 200 || true
read -r WX WY <<< "$(placement 'LibWM Test Window')"
if [[ -z "${WX:-}" ]]; then
    echo "==> could not read the test window placement" >&2
    exit 1
fi
env WAYLAND_DISPLAY="$SOCK" "$VI" \
    --sleep 500 \
    --touch "$((WX + 160))" "$((WY + 120))" \
    --sleep 250 \
    --touch-motion "$((WX + 180))" "$((WY + 140))" \
    --sleep 250 \
    --touch-up \
    --linger 300 >/dev/null 2>&1 || true
sleep 0.3
sde_test_kill "$TOUCH_PID"

sde_test_check "$RUNTIME/touch.log" 'touch down on window' "the shim received wl_touch"
sde_test_check "$RUNTIME/touch.log" 'TestWindow: mouse down at' "the touch became a primary press"
sde_test_check "$RUNTIME/touch.log" 'TestWindow: mouse up at' "the touch became a primary release"

# --- activation -------------------------------------------------------------
env WAYLAND_DISPLAY="$SOCK" LIBWM_TRACE=1 \
    "$BUILD/bin/libwm-activation-test" > "$RUNTIME/activation.log" 2>&1 &
ACTIVATION_PID=$!
SDE_TEST_CLIENT_PIDS+=("$ACTIVATION_PID")
sde_test_wait_log "$RUNTIME/comp.log" "toplevel mapped title='Activation Test A'" 200 || true
read -r AX AY <<< "$(placement 'Activation Test A')"
if [[ -z "${AX:-}" ]]; then
    echo "==> could not read the activation window placement" >&2
    exit 1
fi
env WAYLAND_DISPLAY="$SOCK" "$VI" \
    --move "$((AX + 160))" "$((AY + 120))" \
    --sleep 250 \
    --button 0x110 press \
    --sleep 100 \
    --button 0x110 release \
    --linger 400 >/dev/null 2>&1 || true
sde_test_wait_log "$RUNTIME/comp.log" "xdg-activation focused 'Activation Test B'" 200 || true
sleep 0.3
sde_test_kill "$ACTIVATION_PID"

sde_test_check "$RUNTIME/activation.log" 'ACTIVATION-TEST: window B created' "the click opened the second window"
sde_test_check "$RUNTIME/activation.log" 'requested activation token for window' "the shim requested an activation token"
sde_test_check "$RUNTIME/comp.log" "xdg-activation focused 'Activation Test B'" "the compositor raised and focused the new window"

sde_test_stop_compositor
sde_test_no_fault "$RUNTIME/comp.log" "$RUNTIME/touch.log" "$RUNTIME/activation.log"
sde_test_finish "$RUNTIME/touch.log"
