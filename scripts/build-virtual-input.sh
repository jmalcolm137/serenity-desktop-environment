#!/usr/bin/env bash
# Build the virtual-input test client (wlr virtual keyboard + virtual pointer),
# used by tests that drive the compositor's input. Prints the binary path.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="${TOOLS_BUILD:-$SDE_ROOT/build/input-test}"

mkdir -p "$TOOLS_BUILD"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
VI_XML="$SDE_ROOT/WindowServer/protocol/serenity-virtual-input.xml"

wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
wayland-scanner client-header "$VI_XML" "$TOOLS_BUILD/serenity-virtual-input-client-protocol.h"
wayland-scanner private-code  "$VI_XML" "$TOOLS_BUILD/serenity-virtual-input-protocol.c"

cc -O2 -DSDE_HAVE_VIRTUAL_INPUT -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/serenity-virtual-input-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

echo "$TOOLS_BUILD/virtual-input-test"
