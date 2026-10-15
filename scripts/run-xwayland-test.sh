#!/usr/bin/env bash
# ci-tier: full
# XWayland test (R5): with [Compositor] XWayland left on (default), an X11 client
# runs and maps a window; with XWayland=false the compositor starts no X server
# (no DISPLAY). Runs headless.
set -euo pipefail

# This test starts an X server, and an X server's sockets are *abstract*
# (@/tmp/.X11-unix/X<n>), so they are shared with every other X server on the
# machine and namespaced by the network namespace rather than the filesystem. Two
# Xwaylands fighting over a display name killed a live session's X server, and a
# missing liveness check in the compositor turned that into a crash
# (docs/26-ci.md §6). Re-exec inside a private network namespace so the test's X
# server cannot be seen by — or collide with — a session's.
if [[ "${SDE_TEST_IN_NAMESPACE:-0}" != 1 ]]; then
    if unshare -Urnm true 2>/dev/null; then
        exec unshare -Urnm env SDE_TEST_IN_NAMESPACE=1 "$0" "$@"
    fi
    # No unprivileged namespaces (some containers): only safe with no session.
    if [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/sde" ]]; then
        echo "SKIP: a live session owns the X display and namespaces are unavailable"
        exit 0
    fi
else
    # A private, correctly-owned X socket directory: Xwayland refuses a
    # /tmp/.X11-unix it does not own, and the host's is owned by root, which is an
    # unmapped (nobody) uid inside a user namespace.
    mkdir -p /tmp/.X11-unix
    mount -t tmpfs -o mode=1777 none /tmp/.X11-unix 2>/dev/null || true
fi

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

# The SDE LaunchServer (spawns apps; must adopt the session Xwayland DISPLAY).
LAUNCH_SERVER="$SDE_ROOT/build/LaunchServer/LaunchServer"
"$SDE_ROOT/scripts/build-launch-server.sh" >/dev/null 2>&1 || true

# An X11 client that sets _NET_WM_ICON (xterm/xmessage do not).
XICON="$TOOLS_BUILD/x11-icon-test"
if command -v cc >/dev/null && pkg-config --exists xcb xcb-ewmh; then
    mkdir -p "$TOOLS_BUILD"
    cc -O2 "$SDE_ROOT/tools/x11-icon-test.c" $(pkg-config --cflags --libs xcb xcb-ewmh) -o "$XICON" 2>/dev/null || true
fi

fail=0

run_case() {
    local name="$1" ini="$2"
    local R; R="$(mktemp -d "${TMPDIR:-/tmp}/sde-xwayland-$name.XXXXXX")"
    chmod 700 "$R"
    [[ -n "$ini" ]] && printf '%s\n' "$ini" > "$R/WindowServer.ini"
    local SOCK="sde-xwayland-$name-$$"
    env -u WAYLAND_DISPLAY -u DISPLAY -u SDE_XWAYLAND \
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
# _NET_WM_ICON -> Gfx::Bitmap -> Taskbar/WM, and WM_CLASS.
if [[ -x "$XICON" && -n "$DISP" ]]; then
    env -u WAYLAND_DISPLAY DISPLAY="$DISP" "$XICON" >/dev/null 2>&1 &
    for _ in $(seq 1 40); do grep -qE "X11 window '[^']*' icon 16x16" "$R/comp.log" && break; sleep 0.1; done
    if grep -qE "X11 window '[^']*' icon 16x16" "$R/comp.log"; then
        echo "  ok: the X11 _NET_WM_ICON was fetched (16x16)"
    else
        echo "  FAIL: the X11 window icon was not fetched"; fail=1
    fi
    if grep -qE "WM: X11 window [0-9]+ icon 16x16" "$R/comp.log"; then
        echo "  ok: the X11 window icon was reported to the WindowManager"
    else
        echo "  FAIL: the X11 icon was not reported to the WM"; fail=1
    fi
    if grep -q "class='XIconTest'" "$R/comp.log"; then
        echo "  ok: WM_CLASS was read ('XIconTest')"
    else
        echo "  FAIL: WM_CLASS was not read"; fail=1
    fi
    if grep -q "X11 override-redirect window mapped" "$R/comp.log"; then
        echo "  ok: an override-redirect X11 window mapped (menus/tooltips are unmanaged)"
    else
        echo "  FAIL: the override-redirect window did not map"; fail=1
    fi
    if grep -qE "X11 window [0-9]+ hints: skip_taskbar=false above=true" "$R/comp.log"; then
        echo "  ok: _NET_WM_STATE (always-on-top) was read"
    else
        echo "  FAIL: the _NET_WM_STATE hints were not read"; fail=1
    fi
    if grep -q "reported X11 workarea" "$R/comp.log"; then
        echo "  ok: the workarea was reported to Xwayland (_NET_WORKAREA)"
    else
        echo "  FAIL: no X11 workarea reported"; fail=1
    fi
else
    echo "  note: x11-icon-test unavailable; skipping the icon/class check"
fi
# The LaunchServer spawns apps; it must adopt the session Xwayland DISPLAY (not
# the host's), or X11 apps it spawns open on the host X server.
if [[ -x "$LAUNCH_SERVER" && -n "$DISP" ]]; then
    SDE_STATE_DIR="$R" SDE_SESSION_DIR="$R/session" WAYLAND_DISPLAY="$SOCK" "$LAUNCH_SERVER" > "$R/launch.log" 2>&1 &
    LP=$!
    for _ in $(seq 1 40); do grep -q "using session Xwayland DISPLAY=" "$R/launch.log" && break; sleep 0.1; done
    if grep -q "using session Xwayland DISPLAY=${DISP}" "$R/launch.log"; then
        echo "  ok: the LaunchServer adopted the session DISPLAY ($DISP)"
    else
        echo "  FAIL: the LaunchServer did not adopt the session DISPLAY"; fail=1
    fi
    kill "$LP" 2>/dev/null || true
else
    echo "  note: LaunchServer or DISPLAY unavailable; skipping the display check"
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
    # The override-redirect window (wm_id 0, unmanaged) must NOT be workspace-hid.
    if grep -qE "X11 window 0 workspace 0,0 visible=true" "$R/comp.log"; then
        echo "  ok: the override-redirect window stayed visible across the switch"
    else
        echo "  FAIL: the override-redirect window was hidden by the workspace switch"; fail=1
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
