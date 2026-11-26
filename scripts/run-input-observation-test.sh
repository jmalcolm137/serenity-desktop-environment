#!/usr/bin/env bash
# ci-tier: full
# Headless ext-input-observation-v1 test: a top-left 200x200 layer surface
# observes the pointer. Moving the virtual pointer to (100,100) must report
# (0.5,0.5) and to (400,300), which is outside the surface and over nothing,
# must report (2.0,1.5) as signed 16:16 fixed point. That position is what
# wl_pointer cannot deliver, and what LibWM's global mouse tracking needs.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init input-observation
sde_test_export_roots
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/input-observation-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
mkdir -p "$TOOLS_BUILD"

WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
OBSERVATION_XML="$SDE_ROOT/Services/WindowServer/protocol/ext-input-observation-v1.xml"
SERENITY_XML="$SDE_ROOT/Services/WindowServer/protocol/serenity-window.xml"
wayland-scanner client-header "$OBSERVATION_XML" "$TOOLS_BUILD/ext-input-observation-v1-client-protocol.h"
wayland-scanner private-code "$OBSERVATION_XML" "$TOOLS_BUILD/ext-input-observation-v1-protocol.c"
wayland-scanner client-header "$SERENITY_XML" "$TOOLS_BUILD/serenity-window-client-protocol.h"
wayland-scanner private-code "$SERENITY_XML" "$TOOLS_BUILD/serenity-window-protocol.c"
XDG_XML="$WP/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
wayland-scanner client-header "$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.h"
wayland-scanner private-code "$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/testing/input-observation-test.c" \
    "$TOOLS_BUILD/ext-input-observation-v1-protocol.c" \
    "$TOOLS_BUILD/serenity-window-protocol.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/input-observation-test"

wayland-scanner client-header "$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code "$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code "$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/testing/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

sde_test_compositor LIBWM_WM_PORTAL_PATH="$RUNTIME/wm"

env WAYLAND_DISPLAY="$SOCK" timeout 10 "$TOOLS_BUILD/input-observation-test" > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
SDE_TEST_CLIENT_PIDS+=("$CLIENT_PID")
sleep 1.5
# The first device gives the seat its pointer capability and lets the client
# create the observer; the asserts use the moves that follow.
env WAYLAND_DISPLAY="$SOCK" timeout 5 "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --move 500 500 --sleep 200 >/dev/null 2>&1 || true
sleep 0.5
# Inside the surface: (100,100) of 200x200.
env WAYLAND_DISPLAY="$SOCK" timeout 5 "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --move 100 100 --sleep 200 >/dev/null 2>&1 || true
sleep 0.5
# Outside it, over no surface at all.
env WAYLAND_DISPLAY="$SOCK" timeout 5 "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --move 400 300 --sleep 200 >/dev/null 2>&1 || true
wait "$CLIENT_PID" 2>/dev/null || true

echo "--- client ---"
cat "$RUNTIME/client.log"
echo "--- compositor (observers) ---"
grep -a 'input observer' "$RUNTIME/comp.log" || true

sde_test_check "$RUNTIME/client.log" 'INPUT-OBSERVATION: ok' "positions observed inside and outside the surface"
sde_test_check "$RUNTIME/client.log" 'instance=0 rel=32768,32768' "inside: (100,100) is (0.5,0.5)"
sde_test_check "$RUNTIME/client.log" 'instance=0 rel=131072,98304' "outside and unfocused: (400,300) is (2.0,1.5)"
sde_test_check "$RUNTIME/comp.log" 'input observer for surface' "the compositor created the observer"
sde_test_case_stop
sde_test_no_fault "$RUNTIME/comp.log"
sde_test_finish "$RUNTIME/client.log"
