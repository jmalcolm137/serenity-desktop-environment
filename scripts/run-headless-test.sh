#!/usr/bin/env bash
# R1 smoke test: run the SDE compositor with the HEADLESS backend and confirm it
# hosts the three surface kinds the milestone calls for:
#
#   1. a LibWindowServer (Serenity) application  -> an xdg toplevel
#   2. a layer-shell panel                        -> a layer surface
#   3. a native (non-Serenity) Wayland application-> an xdg toplevel
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/headless-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
LAYER_XML="$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$LAYER_XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$LAYER_XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c"
wayland-scanner client-header "$XDG_XML"   "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML"   "$TOOLS_BUILD/xdg-shell-protocol.c"
SW_XML="$SDE_ROOT/WindowServer/protocol/serenity-window.xml"
wayland-scanner client-header "$SW_XML"    "$TOOLS_BUILD/serenity-window-client-protocol.h"
wayland-scanner private-code  "$SW_XML"    "$TOOLS_BUILD/serenity-window-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/layer-shell-test.c" \
    "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/layer-shell-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/native-app-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/serenity-window-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/native-app-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-headless.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-headless-$$"

# No WAYLAND_DISPLAY/DISPLAY so wlr_backend_autocreate uses the requested backend.
# SDE_WM_PORTAL_PATH keeps the WindowManagerServer socket out of /tmp/portal/wm,
# so the test cannot disturb a live session.
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" -s "$BUILD/bin/TextEditor" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/layer-shell-test" > "$RUNTIME/layer.log" 2>&1 || true
env WAYLAND_DISPLAY="$SOCK" timeout 4 "$TOOLS_BUILD/native-app-test" > "$RUNTIME/native.log" 2>&1 || true
# Give the LibWindowServer app a moment to map its toplevel.
sleep 1

# Stop the shell app the compositor spawned (it is a child of the compositor),
# then the compositor itself.
pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
cat "$RUNTIME/comp.log"
echo "--- layer-shell client ---"
cat "$RUNTIME/layer.log"
echo "--- native client ---"
cat "$RUNTIME/native.log"

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

echo "=== R1 headless smoke test ==="
check "output 'HEADLESS-1' 1280x720"
check "layer surface namespace='sde-test-panel'"
check "toplevel mapped title='SDE Native Test'"
check "toplevel mapped title='Untitled[*] - Text Editor'"

if [[ "$fail" -ne 0 ]]; then
    echo "==> R1 headless smoke test FAILED"
    exit 1
fi
echo "==> R1 headless smoke test PASSED"
