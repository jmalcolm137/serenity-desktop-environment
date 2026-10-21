#!/usr/bin/env bash
# Headless keyboard-replug test for LibGUI clients.
#
# run-keyboard-hotplug-test.sh covers the compositor's side of a keyboard going
# away and coming back, with a raw Wayland client. This covers a LibGUI client
# (LibWM): when the last keyboard goes, the seat withdraws the keyboard
# capability and the client's wl_keyboard becomes inert; when a keyboard comes
# back, the client must ask for a new wl_keyboard, or it never gets the keymap,
# the enter or a single key again. (Found as an applet popup that would not take
# typing after the virtual keyboard reconnected.)
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init keyboard-replug
sde_test_export_roots
TEST="$SDE_ROOT/build/test/libwm-keyboard-replug-test"
KEY_A=30
KEY_B=48

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tests.sh" libwm-keyboard-replug-test >/dev/null
VI="$(sde_test_vi)"

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"

# How often the client has reported a seat without a usable keyboard: once at
# startup (no keyboard exists yet), and once more each time it lets go of one.
keyboardless() { grep -cF "(pointer=true, keyboard=false)" "$RUNTIME/client.log" 2>/dev/null || true; }

sde_test_client env WAYLAND_DISPLAY="$SOCK" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$TEST" --duration 20000 > "$RUNTIME/client.log" 2>&1
sde_test_wait_log "$RUNTIME/client.log" "REPLUG-TEST: shown" || true

# The first keyboard: a warm-up Shift (the first press may race the capability
# change), then "a". Its process exits afterwards, which unplugs it.
env WAYLAND_DISPLAY="$SOCK" "$VI" \
    --key 42 press --key 42 release --sleep 500 \
    --key "$KEY_A" press --key "$KEY_A" release --linger 300 > "$RUNTIME/vi1.log" 2>&1 || true
sde_test_wait_log "$RUNTIME/client.log" "text='a'" 30 || true
# The capability went away with it, and the client let go of its wl_keyboard.
for _ in $(seq 1 30); do
    [[ "$(keyboardless)" -ge 2 ]] && break
    sleep 0.1
done

# A new keyboard: the same warm-up, then "b".
env WAYLAND_DISPLAY="$SOCK" "$VI" \
    --key 42 press --key 42 release --sleep 500 \
    --key "$KEY_B" press --key "$KEY_B" release --linger 300 > "$RUNTIME/vi2.log" 2>&1 || true
sde_test_wait_log "$RUNTIME/client.log" "text='ab'" 30 || true

echo "--- client ---"
grep -E "REPLUG-TEST|seat capabilities" "$RUNTIME/client.log" || true

echo "=== keyboard replug (LibGUI client) ==="
sde_test_check "$RUNTIME/client.log" "text='a'" "a key typed on the first keyboard arrives"
if [[ "$(keyboardless)" -ge 2 ]]; then
    echo "  ok: the client drops its wl_keyboard when the capability goes"
else
    echo "  FAIL: the client kept its wl_keyboard after the capability went"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_check "$RUNTIME/client.log" "text='ab'" "a key typed on a keyboard plugged in later arrives too"
sde_test_no_fault "$RUNTIME/comp.log" "$RUNTIME/client.log"
sde_test_finish "$RUNTIME/client.log"
