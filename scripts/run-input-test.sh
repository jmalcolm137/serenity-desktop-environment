#!/usr/bin/env bash
# Headless virtual-input test (docs/09-r3-implementation-plan.md §2.4): script a
# pointer right-click on a window's titlebar through the wlroots virtual-pointer
# protocol and assert the compositor opens the window menu. This is the harness
# R3 behaviours are scripted with; keyboard injection is available too.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init input
sde_test_export_roots
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
SW_XML="$SDE_ROOT/Services/WindowServer/protocol/serenity-window.xml"
wayland-scanner client-header "$SW_XML" "$TOOLS_BUILD/serenity-window-client-protocol.h"
wayland-scanner private-code  "$SW_XML" "$TOOLS_BUILD/serenity-window-protocol.c"
# native-app-test also speaks presentation-time, fractional-scale and xdg-decoration.
PRES_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/presentation-time/presentation-time.xml"
FRAC_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/staging/fractional-scale/fractional-scale-v1.xml"
DECO_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
wayland-scanner client-header "$PRES_XML" "$TOOLS_BUILD/presentation-time-client-protocol.h"
wayland-scanner private-code  "$PRES_XML" "$TOOLS_BUILD/presentation-time-protocol.c"
wayland-scanner client-header "$FRAC_XML" "$TOOLS_BUILD/fractional-scale-v1-client-protocol.h"
wayland-scanner private-code  "$FRAC_XML" "$TOOLS_BUILD/fractional-scale-v1-protocol.c"
wayland-scanner client-header "$DECO_XML" "$TOOLS_BUILD/xdg-decoration-client-protocol.h"
wayland-scanner private-code  "$DECO_XML" "$TOOLS_BUILD/xdg-decoration-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"

cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/native-app-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/serenity-window-protocol.c" \
    "$TOOLS_BUILD/presentation-time-protocol.c" \
    "$TOOLS_BUILD/fractional-scale-v1-protocol.c" \
    "$TOOLS_BUILD/xdg-decoration-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/native-app-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"

SOCK="sde-input-$$"

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm" WINDOWSERVER_DUMP_THUMBNAIL="$RUNTIME"
# A normal client to have a titlebar to click on. It must outlive the whole
# sequence below: every later step assumes a window exists, and one that exits
# early fails in confusing ways -- with no window left the window switcher's list is
# empty, so it returns before logging, and the switcher checks report a missing
# line rather than a missing window. At 8s the client died between the Alt+Tab and
# Super+Tab legs.
env WAYLAND_DISPLAY="$SOCK" setsid timeout 30 "$TOOLS_BUILD/native-app-test" --server-side-decorations > "$RUNTIME/native.log" 2>&1 &
sleep 1

# One virtual-input session for the whole sequence. The tool creates its pointer and
# keyboard when it connects and destroys them when it exits, and the seat drops its
# keyboard capability -- and with it every client's keyboard focus -- whenever the
# last keyboard goes (update_seat_capabilities). A device per step therefore hands
# each step a client that has just lost focus, and a key sent in that window goes to
# nobody: that is the flake this test used to fail with ("the Alt release after
# Alt+Tab (client saw 0 key events)"). Holding one session keeps the devices up, so
# the client binds once and every key is delivered.
#
#   1. right-click the titlebar (the frame's top inset) -> the window menu opens
#   2. Escape dismisses it, so the keybindings below are not swallowed
#   3. Super+Left tiles the focused window to the left half (WS2)
#   4. drag the titlebar to the right edge and release: snap tiles it right (WS2)
#   5. Alt+Tab opens the switcher; releasing Alt activates the selection (WS3)
#   6. Super+Tab opens it in all-workspaces mode (WS3)
#   7. Ctrl+Alt+Right switches to the next workspace stack (WS1)
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 \
    --move 160 110 --button 0x111 press --button 0x111 release --sleep 700 \
    --key 1 press --key 1 release --sleep 700 \
    --key 125 press --key 105 press --key 105 release --key 125 release --sleep 700 \
    --move 60 10 --button 0x110 press --move 1275 300 --move 1275 301 --button 0x110 release --sleep 700 \
    --key 56 press --key 15 press --key 15 release --key 56 release --sleep 700 \
    --key 125 press --key 15 press --key 15 release --key 125 release --sleep 700 \
    --key 29 press --key 56 press --key 106 press --key 106 release --key 56 release --key 29 release --sleep 700 \
    --linger 1000 \
    > "$RUNTIME/input.log" 2>&1 || true

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
    # Which events it did see: press-vs-release is the whole question here.
    grep 'native-app-test: key ' "$RUNTIME/native.log" 2>/dev/null | head -4 | sed 's/^/      /' || true
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
