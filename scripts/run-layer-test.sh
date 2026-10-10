#!/usr/bin/env bash
# Verify WindowServer's layer-shell exclusive zones: a 30px TOP panel
# anchored to the bottom must shrink the output's usable area from 720 to 690.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
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
    "$SDE_ROOT/tools/layer-shell-test.c" \
    "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/layer-shell-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-layer.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-layer-$$"
env WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$LIBGUI_WAYLAND_ROOT/serenity/Base/res}" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 50); do [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break; sleep 0.1; done

env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/layer-shell-test" > "$RUNTIME/client.log" 2>&1 || true
sleep 0.3
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
cat "$RUNTIME/comp.log"
echo "--- client log ---"
cat "$RUNTIME/client.log"

if grep -q "usable area for 'WL-1': 1280x690 at 0,0" "$RUNTIME/comp.log"; then
    echo "==> layer-shell exclusive zone test PASSED"
else
    echo "==> layer-shell exclusive zone test FAILED (expected usable 1280x690)"
    exit 1
fi
