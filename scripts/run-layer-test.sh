#!/usr/bin/env bash
# Verify WindowServer's layer-shell exclusive zones: a 30px TOP panel
# anchored to the bottom must shrink the output's usable area from 720 to 690.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init layer
sde_test_export_roots
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/layer-test"
XML="$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.h"
wayland-scanner private-code "$XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.c"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/layer-shell-test.c" \
    "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/layer-shell-test"

sde_test_compositor

env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/layer-shell-test" > "$RUNTIME/client.log" 2>&1 || true
sleep 0.3
sde_test_stop_compositor

echo "--- compositor log ---"
cat "$RUNTIME/comp.log"
echo "--- client log ---"
cat "$RUNTIME/client.log"

# The headless output is 'HEADLESS-1' (the nested backend used to call it 'WL-1',
# which is what this check was written against).
echo "=== layer-shell exclusive zone ==="
sde_test_check "$RUNTIME/comp.log" "usable area for 'HEADLESS-1': 1280x690 at 0,0" "a 30px exclusive zone leaves 1280x690 usable"
sde_test_finish "$RUNTIME/client.log"
