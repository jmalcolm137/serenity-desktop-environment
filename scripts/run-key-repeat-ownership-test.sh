#!/usr/bin/env bash
# Key-repeat ownership.
#
# wl_keyboard leaves repeat to the client (repeat_info only advertises the rate).
# This compositor repeats held keys itself, which it declares to clients in the
# host settings as `keyboard.repeat=compositor`; LibWM must then stand down, or a
# held key repeats twice as fast.
#
# The test holds a key on a LibWM application for a fixed time and counts the
# presses it receives: about RepeatRate/s (the compositor's), not twice that.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init key-repeat-ownership
sde_test_export_roots
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/input-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
ninja -C "$BUILD" libwm-test-window >/dev/null

mkdir -p "$TOOLS_BUILD"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/testing/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
APP_PID=""

# Fast repeat, so the test is short: 200 ms delay, then one press every 50 ms.
cat > "$RUNTIME/input.ini" <<'EOF'
[Keyboard]
Layouts=us
RepeatDelay=200
RepeatRate=20
EOF

sde_test_compositor LIBWM_WM_PORTAL_PATH="$RUNTIME/wm"

env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" LIBWM_TRACE=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$BUILD/bin/libwm-test-window" > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!

# Wait until the window is up (it has been presented) before typing into it.
for _ in $(seq 1 100); do
    grep -q 'presented window' "$RUNTIME/app.log" 2>/dev/null && break
    sleep 0.1
done

# Hold KEY_A (30) for 1200 ms: after the 200 ms delay that is ~20 presses.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 30 press --sleep 1200 --key 30 release > "$RUNTIME/input.log" 2>&1 || true
sleep 0.3

kill "$APP_PID" 2>/dev/null || true
wait "$APP_PID" 2>/dev/null || true
APP_PID=""
sde_test_stop_compositor 2>/dev/null || true

presses="$(grep -c 'key down code_point=97' "$RUNTIME/app.log" 2>/dev/null || true)"
presses="${presses:-0}"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== key-repeat ownership ==="
echo "  presses received in 1200 ms: $presses (20/s would be ~20, doubled ~40)"
grep -E "repeat_info|keyboard repeat|key down code_point=97" "$RUNTIME/app.log" 2>/dev/null | head -3 || true

check "the shim was told the compositor repeats" "grep -q \"keyboard repeat is the compositor's\" \"$RUNTIME/app.log\""
check "the held key repeated"                   "[[ '$presses' -ge 6 ]]"
check "it did not repeat twice as fast"         "[[ '$presses' -le 30 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- app log (tail) ---"; tail -15 "$RUNTIME/app.log"
    echo "==> key-repeat ownership FAILED"
    exit 1
fi
echo "==> key-repeat ownership PASSED"
