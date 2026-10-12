#!/usr/bin/env bash
# Headless virtual-input test (docs/09-r3-implementation-plan.md §2.4): script a
# pointer right-click on a window's titlebar through the wlroots virtual-pointer
# protocol and assert the compositor opens the window menu. This is the harness
# R3 behaviours are scripted with; keyboard injection is available too.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/input-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"

wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
# serenity-window: the private chrome protocol, so native-app-test can send an icon.
SW_XML="$SDE_ROOT/WindowServer/protocol/serenity-window.xml"
wayland-scanner client-header "$SW_XML" "$TOOLS_BUILD/serenity-window-client-protocol.h"
wayland-scanner private-code  "$SW_XML" "$TOOLS_BUILD/serenity-window-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"

cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/native-app-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/serenity-window-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/native-app-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-input.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$ "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$.lock 2>/dev/null || true' EXIT

SOCK="sde-input-$$"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    WINDOWSERVER_DUMP_THUMBNAIL="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

# A normal client to have a titlebar to click on.
env WAYLAND_DISPLAY="$SOCK" setsid timeout 8 "$TOOLS_BUILD/native-app-test" > "$RUNTIME/native.log" 2>&1 &
sleep 1

# Right-click the titlebar (the frame's top inset) through a virtual pointer.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 \
    --move 60 10 \
    --button 0x111 press \
    --button 0x111 release \
    > "$RUNTIME/input.log" 2>&1 || true
sleep 1

# Dismiss the window menu (Escape = evdev keycode 1) so the keyboard bindings
# below are not swallowed by it.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 1 press --key 1 release --sleep 200 \
    > "$RUNTIME/escape.log" 2>&1 || true
sleep 1

# Super+Left tiles the focused window to the left half (WS2).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 125 press --key 105 press \
    --key 105 release --key 125 release \
    --sleep 200 \
    > "$RUNTIME/tile.log" 2>&1 || true
sleep 1

# Drag the titlebar to the right screen edge and release: drag-to-edge snapping
# previews the target and tiles on release (WS2, WindowTileType::Right == 2).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --move 60 10 --button 0x110 press \
    --move 1275 300 --move 1275 301 \
    --button 0x110 release --sleep 200 \
    > "$RUNTIME/drag.log" 2>&1 || true
sleep 1

# Alt+Tab opens the window switcher (current workspace); releasing Alt activates
# the selection (WS3).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 56 press --key 15 press --key 15 release --key 56 release \
    --sleep 200 \
    > "$RUNTIME/switcher.log" 2>&1 || true
sleep 1

# Super+Tab opens it in all-workspaces mode (WS3).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 125 press --key 15 press --key 15 release --key 125 release \
    --sleep 200 \
    > "$RUNTIME/switcher-all.log" 2>&1 || true
sleep 1

# Ctrl+Alt+Right switches to the next workspace (WS1).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 29 press --key 56 press --key 106 press \
    --key 106 release --key 56 release --key 29 release \
    --sleep 200 \
    > "$RUNTIME/ws.log" 2>&1 || true
sleep 1

kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
cat "$RUNTIME/comp.log"
echo "--- input client ---"
cat "$RUNTIME/input.log"

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

echo "=== virtual-input test ==="
check "virtual pointer connected"
check "toplevel mapped title='SDE Native Test'"
check "composed window icon 16x16"
check "window menu opened for wm_id="
# WS1: Ctrl+Alt+Right switched to the next workspace stack.
check "switched to workspace 0,1"
# WS2: Super+Left tiled the focused window (WindowTileType::Left == 1).
check "tiled wm_id=1 as 1"
# WS2: dragging the titlebar to the right edge tiled it right (Right == 2).
check "tiled wm_id=1 as 2"
# WS3: Alt+Tab listed the current workspace's windows and releasing the modifier
# activated the selection.
check "WindowSwitcher: showing 1 windows (mode=0)"
check "WindowSwitcher: showing 1 windows (mode=1)"
check "WindowSwitcher: activating wm_id=1"
# Alt+Tab must not leave the client's Alt key stuck down: the client received the
# Alt press (it may have opened its menu bar), so it must receive the release too
# (keycode 56 = KEY_LEFTALT), or typing stops reaching the text area.
if grep -qF "key 56 release" "$RUNTIME/native.log"; then
    echo "  ok: the Alt release reached the focused client after Alt+Tab"
else
    echo "  MISSING: Alt release after Alt+Tab (client saw $(grep -c 'native-app-test: key ' "$RUNTIME/native.log" 2>/dev/null) key events)"
    fail=1
fi
# The thumbnail-capture spike (docs/09 §3): the toplevel's pixels were read back
# into a bitmap of the client's size.
check "thumbnail for wm_id=1 is 320x200"
if [[ -f "$RUNTIME/thumb-1.png" ]]; then
    echo "  ok: thumbnail PNG written"
else
    echo "  MISSING: thumbnail PNG"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> virtual-input test FAILED"
    exit 1
fi
echo "==> virtual-input test PASSED"
