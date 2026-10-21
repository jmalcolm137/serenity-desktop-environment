#!/usr/bin/env bash
# ci-tier: full
# Headless xdg-toplevel-drag-v1 test: a client creates a data source, a
# toplevel-drag object and attaches its toplevel, then starts a drag; the
# compositor must move the toplevel with the cursor (cursor - offset) and the
# drag must end cleanly on release.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init toplevel-drag
sde_test_export_roots
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/toplevel-drag-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
mkdir -p "$TOOLS_BUILD"

WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
XDG_XML="$WP/stable/xdg-shell/xdg-shell.xml"
DRAG_XML="$WP/staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
wayland-scanner client-header "$DRAG_XML" "$TOOLS_BUILD/xdg-toplevel-drag-v1-client-protocol.h"
wayland-scanner private-code "$DRAG_XML" "$TOOLS_BUILD/xdg-toplevel-drag-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/toplevel-drag-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/xdg-toplevel-drag-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/toplevel-drag-test"

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

env WAYLAND_DISPLAY="$SOCK" timeout 10 "$TOOLS_BUILD/toplevel-drag-test" > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
SDE_TEST_CLIENT_PIDS+=("$CLIENT_PID")
sleep 1.5
env WAYLAND_DISPLAY="$SOCK" timeout 8 "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 \
    --move 200 200 --sleep 300 \
    --button 0x110 press --sleep 300 \
    --move 300 300 --sleep 300 \
    --button 0x110 release --sleep 300 >/dev/null 2>&1 || true
sleep 1
kill "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true

echo "--- client ---"
cat "$RUNTIME/client.log"
echo "--- compositor (drag) ---"
grep -a 'xdg-toplevel-drag' "$RUNTIME/comp.log" || true

sde_test_check "$RUNTIME/comp.log" "xdg-toplevel-drag attached 'toplevel-drag-test' at offset 50,50" "the toplevel was attached with the requested offset"
sde_test_check "$RUNTIME/comp.log" 'xdg-toplevel-drag started' "the drag started"
sde_test_check "$RUNTIME/comp.log" 'xdg-toplevel-drag moved toplevel to 150,150' "the toplevel followed the cursor at start"
sde_test_check "$RUNTIME/comp.log" 'xdg-toplevel-drag moved toplevel to 250,250' "the toplevel followed the drag motion"
sde_test_case_stop
sde_test_no_fault "$RUNTIME/comp.log"
sde_test_finish "$RUNTIME/client.log"
