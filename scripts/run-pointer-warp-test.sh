#!/usr/bin/env bash
# ci-tier: full
# Headless wp-pointer-warp-v1 test: a top-layer surface receives pointer focus,
# asks the compositor to warp the pointer to (50,50), and must then receive the
# motion event there. The virtual pointer provides the initial focus.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init pointer-warp
sde_test_export_roots
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/pointer-warp-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
mkdir -p "$TOOLS_BUILD"

WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
wayland-scanner client-header "$WP/staging/pointer-warp/pointer-warp-v1.xml" "$TOOLS_BUILD/pointer-warp-v1-client-protocol.h"
wayland-scanner private-code "$WP/staging/pointer-warp/pointer-warp-v1.xml" "$TOOLS_BUILD/pointer-warp-v1-protocol.c"
wayland-scanner client-header "$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.h"
wayland-scanner private-code "$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c"
XDG_XML="$WP/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/pointer-warp-test.c" \
    "$TOOLS_BUILD/pointer-warp-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/pointer-warp-test"

wayland-scanner client-header "$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code "$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code "$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"

env WAYLAND_DISPLAY="$SOCK" timeout 10 "$TOOLS_BUILD/pointer-warp-test" > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
SDE_TEST_CLIENT_PIDS+=("$CLIENT_PID")
sleep 1.5
# The first device establishes the pointer capability while the cursor is away
# from the layer surface; the second moves onto it, producing a fresh enter (and
# therefore a serial the client can quote) for the now-existing wl_pointer.
env WAYLAND_DISPLAY="$SOCK" timeout 5 "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --move 500 500 --sleep 200 >/dev/null 2>&1 || true
sleep 0.5
env WAYLAND_DISPLAY="$SOCK" timeout 5 "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --move 100 100 --sleep 300 >/dev/null 2>&1 || true
wait "$CLIENT_PID" 2>/dev/null || true

echo "--- client ---"
cat "$RUNTIME/client.log"
echo "--- compositor (warp) ---"
grep -a 'pointer warp' "$RUNTIME/comp.log" || true

sde_test_check "$RUNTIME/client.log" 'POINTER-WARP: ok' "the pointer warped to the requested position"
sde_test_check "$RUNTIME/comp.log" 'pointer warp to 50\.0,50\.0' "the compositor logged the warp"
sde_test_case_stop
sde_test_no_fault "$RUNTIME/comp.log"
sde_test_finish "$RUNTIME/client.log"
