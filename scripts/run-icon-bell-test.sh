#!/usr/bin/env bash
# Functional test for the standard-protocol parity additions (docs/21):
# xdg-toplevel-icon-v1 (a client sets a window icon) and xdg-system-bell-v1 (a
# client rings the system bell). Runs headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init icon-bell
sde_test_export_roots
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

TOOLS_BUILD="$SDE_ROOT/build/icon-bell-test"
mkdir -p "$TOOLS_BUILD"
scan() { # scan <xml> <prefix>
    wayland-scanner client-header "$1" "$TOOLS_BUILD/$2-client-protocol.h"
    wayland-scanner private-code "$1" "$TOOLS_BUILD/$2-client-protocol.c"
}
scan "$WP/stable/xdg-shell/xdg-shell.xml" xdg-shell
scan "$WP/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml" xdg-toplevel-icon-v1
scan "$WP/staging/xdg-system-bell/xdg-system-bell-v1.xml" xdg-system-bell-v1
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/xdg-icon-bell-test.c" \
    "$TOOLS_BUILD/xdg-shell-client-protocol.c" \
    "$TOOLS_BUILD/xdg-toplevel-icon-v1-client-protocol.c" \
    "$TOOLS_BUILD/xdg-system-bell-v1-client-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/xdg-icon-bell-test"

sde_test_compositor
sleep 0.5

echo "=== a client sets an icon and rings the bell ==="
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/xdg-icon-bell-test" > "$RUNTIME/tool.log" 2>&1 || true
sleep 0.3

sde_test_check "$RUNTIME/comp.log" "xdg-toplevel-icon set" "the client's xdg-toplevel-icon-v1 icon was applied"
sde_test_check "$RUNTIME/comp.log" "xdg-system-bell ring" "the client's xdg-system-bell-v1 ring was handled"
sde_test_no_fault
sde_test_finish "$RUNTIME/tool.log"
