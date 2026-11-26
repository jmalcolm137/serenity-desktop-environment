#!/usr/bin/env bash
# Primary selection from a Serenity application to native toolkits.
#
# SerenityOS has one clipboard; native toolkits also have PRIMARY (the
# middle-click selection). LibWM publishes a Serenity copy as both CLIPBOARD and
# PRIMARY, so a native application can middle-click paste it. wlroots only
# accepts set_selection with a serial from a recent input event, so the test
# plugs in a virtual keyboard first: the application's window is focused and
# receives a serial, exactly as it would on a real seat.
#
# The other direction (a native PRIMARY read by a Serenity application, with no
# CLIPBOARD owner) is covered by libgui-wayland's own clipboard suite, whose test
# compositor does not validate serials; this test covers the compositor that
# does.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init primary-selection
sde_test_export_roots
# shellcheck source=../../libgui-wayland/scripts/lib/build-peer.sh
. "$LIBGUI_WAYLAND_ROOT/scripts/lib/build-peer.sh"

WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/input-test"
APP_PID=""
PEER_PID=""
VK_PID=""

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
ninja -C "$BUILD" libwm-clipboard-test >/dev/null
PEER="$RUNTIME/wl-clipboard-peer"
build_wl_clipboard_peer "$PEER" >/dev/null

# virtual-input-test brings up a keyboard; a seat with no devices has no serials
# and a compliant compositor rejects selection requests.
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
sde_test_compositor LIBWM_WM_PORTAL_PATH="$RUNTIME/wm"

env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" --linger 20000 > "$RUNTIME/keyboard.log" 2>&1 &
VK_PID=$!
sleep 0.5

# The application copies text; its window has keyboard focus, so the input
# serial it uses is a real one.
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" LIBWM_TRACE=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$BUILD/bin/libwm-clipboard-test" windowed > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!
for _ in $(seq 1 100); do grep -q "wrote text" "$RUNTIME/app.log" 2>/dev/null && break; sleep 0.1; done
sleep 0.4

# A native client reads PRIMARY: it must see the application's copy.
env WAYLAND_DISPLAY="$SOCK" timeout 5 "$PEER" --primary get text/plain > "$RUNTIME/get-primary.out" 2>&1 || true

wait "$APP_PID" 2>/dev/null || true
APP_PID=""
kill "$VK_PID" "$PEER_PID" 2>/dev/null || true
wait "$VK_PID" 2>/dev/null || true
sde_test_stop_compositor 2>/dev/null || true

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== primary selection ==="
check "the application's copy is readable as PRIMARY" "grep -q 'libwm-clipboard-hello' \"$RUNTIME/get-primary.out\""
check "it was published with an input serial"          "grep -q 'set primary selection' \"$RUNTIME/app.log\""

if [[ "$fail" -ne 0 ]]; then
    echo "--- app log (primary/clipboard lines) ---"
    grep -E "primary|clipboard|seat capabilities|keyboard" "$RUNTIME/app.log" || cat "$RUNTIME/app.log"
    echo "--- peer get ---"; cat "$RUNTIME/get-primary.out"
    echo "==> primary selection FAILED"
    exit 1
fi
echo "==> primary selection PASSED"
