#!/usr/bin/env bash
# XWayland test (R5): with [Compositor] XWayland left on (default), an X11 client
# runs and maps a window; with XWayland=false the compositor starts no X server
# (no DISPLAY). Runs headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

command -v xmessage >/dev/null || { echo "SKIP: no xmessage (X11 client)"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

# The virtual-input harness (a virtual keyboard) so we can drive Alt+Tab.
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/xwayland-test"
VIT="$TOOLS_BUILD/virtual-input-test"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
if [[ -f "$VK_XML" && -f "$VP_XML" ]] && command -v wayland-scanner >/dev/null && command -v cc >/dev/null; then
    mkdir -p "$TOOLS_BUILD"
    wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
    wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
    wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
    wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
    cc -O2 -I "$TOOLS_BUILD" \
        "$SDE_ROOT/tools/virtual-input-test.c" \
        "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
        "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
        $(pkg-config --cflags --libs wayland-client xkbcommon) \
        -o "$VIT" 2>/dev/null || true
fi

# The standard-workspace client, to switch stacks (R4.8).
WS="$SDE_ROOT/build/tools/ext-workspace-test"
[[ -x "$WS" ]] || "$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true

fail=0

run_case() {
    local name="$1" ini="$2"
    local R; R="$(mktemp -d "${TMPDIR:-/tmp}/sde-xwayland-$name.XXXXXX")"
    chmod 700 "$R"
    [[ -n "$ini" ]] && printf '%s\n' "$ini" > "$R/WindowServer.ini"
    local SOCK="sde-xwayland-$name-$$"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        SDE_STATE_DIR="$R" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
    local CP=$!
    for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
    sleep 0.5
    local DISP=""
    [[ -f "$R/xwayland-display" ]] && DISP="$(cat "$R/xwayland-display")"
    if [[ -n "$DISP" ]]; then
        env -u WAYLAND_DISPLAY DISPLAY="$DISP" timeout 20 xmessage -name xwayland-test "hello" >/dev/null 2>&1 &
        sleep 3
    fi
    echo "$R $CP $DISP $SOCK"
}

echo "=== enabled (default) ==="
read -r R CP DISP SOCK <<<"$(run_case on "")"
if grep -q "Xwayland enabled" "$R/comp.log"; then echo "  ok: Xwayland enabled (DISPLAY=$DISP)"; else echo "  FAIL: Xwayland not enabled"; fail=1; fi
if grep -q "Xwayland ready" "$R/comp.log"; then echo "  ok: Xwayland started"; else echo "  FAIL: Xwayland never started"; fail=1; fi
if grep -q "X11 window mapped 'xwayland-test'" "$R/comp.log"; then echo "  ok: the X11 client mapped a window"; else echo "  FAIL: no X11 window mapped"; fail=1; fi
if grep -q "X11 frame 'xwayland-test'" "$R/comp.log"; then echo "  ok: the X11 window got a server-side decoration frame"; else echo "  FAIL: no X11 decoration frame"; fail=1; fi
if grep -q "WM: X11 window" "$R/comp.log"; then echo "  ok: the X11 window was reported to the WindowManager (Taskbar)"; else echo "  FAIL: no X11 WM emission"; fail=1; fi
# Alt+Tab must list the X11 window and activate it (WindowRef; docs/20 item 1).
# evdev keycodes: LeftAlt=56, Tab=15. Alt+Tab opens the switcher; releasing Alt
# activates the selection.
if [[ -x "$VIT" ]]; then
    env WAYLAND_DISPLAY="$SOCK" "$VIT" --extent 1280 720 \
        --key 56 press --key 15 press --key 15 release --key 56 release >/dev/null 2>&1 || true
    sleep 0.5
    if grep -q "WindowSwitcher: showing 1 windows" "$R/comp.log"; then
        echo "  ok: Alt+Tab lists the X11 window in the switcher"
    else
        echo "  FAIL: the X11 window was not listed by the switcher"; fail=1
    fi
    if grep -q "WindowSwitcher: activating X11 window wm_id=" "$R/comp.log"; then
        echo "  ok: releasing Alt activates the X11 window"
    else
        echo "  FAIL: the switcher did not activate the X11 window"; fail=1
    fi
else
    echo "  note: virtual-input harness unavailable; skipping the Alt-Tab check"
fi
# X11 windows join the workspace grid (docs/20 item 2): they map on the current
# stack and hide/show with it.
if [[ -x "$WS" ]]; then
    if grep -qE "X11 window [0-9]+ workspace 0,0 visible=true" "$R/comp.log"; then
        echo "  ok: the X11 window is on the current workspace (0,0)"
    else
        echo "  FAIL: the X11 window was not placed on the workspace grid"; fail=1
    fi
    env WAYLAND_DISPLAY="$SOCK" "$WS" 2 >/dev/null 2>&1 || true
    for _ in $(seq 1 40); do grep -qF "switched to workspace 0,1" "$R/comp.log" && break; sleep 0.1; done
    if grep -qF "switched to workspace 0,1" "$R/comp.log"; then
        echo "  ok: switched to workspace 0,1 (ext-workspace-v1)"
    else
        echo "  FAIL: the workspace did not switch"; fail=1
    fi
    if grep -qE "X11 window [0-9]+ workspace 0,0 visible=false" "$R/comp.log"; then
        echo "  ok: the X11 window hid with its workspace"
    else
        echo "  FAIL: the X11 window did not hide on a workspace switch"; fail=1
    fi
else
    echo "  note: ext-workspace-test unavailable; skipping the workspace check"
fi
if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$R/comp.log"; then echo "  FAIL: the compositor faulted"; fail=1; fi
kill "$CP" 2>/dev/null || true; wait "$CP" 2>/dev/null || true
rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR"/sde-xwayland-on-$$* 2>/dev/null || true

echo "=== disabled ([Compositor] XWayland=false) ==="
read -r R CP DISP _SOCK <<<"$(run_case off "[Compositor]
XWayland=false")"
if grep -q "Xwayland enabled" "$R/comp.log"; then echo "  FAIL: Xwayland enabled despite XWayland=false"; fail=1; else echo "  ok: Xwayland not enabled"; fi
if [[ -f "$R/xwayland-display" ]]; then echo "  FAIL: DISPLAY written despite XWayland=false"; fail=1; else echo "  ok: no X server / DISPLAY"; fi
kill "$CP" 2>/dev/null || true; wait "$CP" 2>/dev/null || true
rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR"/sde-xwayland-off-$$* 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then echo "==> xwayland test FAILED"; exit 1; fi
echo "==> xwayland test PASSED"
