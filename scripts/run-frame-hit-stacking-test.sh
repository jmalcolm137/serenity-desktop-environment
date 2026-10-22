#!/usr/bin/env bash
# Frame-press regression: the window *under the pointer* takes the press.
#
# hit_test_frame() (and its X11 twin) scanned the toplevel array in creation order
# and returned the first window whose frame rectangle contained the point. That
# array is never reordered, so a window created later and then occluded still
# matched -- and when the point fell inside its hidden titlebar the press began an
# interactive *move* of that window. Reported as "trying to click a tab in Firefox
# drags the Terminal that is running behind it", with the Terminal not even visible.
#
# The case needs two things at once, which is why it survived so long:
#   * the occluded window must have been created *later* (so it comes first in the
#     backwards scan), and
#   * the point must be inside the occluded window's titlebar band.
#
# So: map L, map O (O is on top and later in the array), click L's body to raise it
# (L is now on top but still *earlier* in the array -- nothing reorders it), then
# press a point that lies in O's titlebar band and inside L's frame. L is what the
# user sees there, so L must keep the press: O must not be focused, and L must not
# be deactivated. Before the fix O was scanned first, took the press on its hidden
# titlebar, and was focused and dragged.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
TOOLS_BUILD="$SDE_ROOT/build/frame-hit-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$NATIVE" ]] || "$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

# The input tool (wlroots virtual pointer + keyboard), built here so the test is
# self-contained -- same recipe as run-terminal-mouse-test.sh.
mkdir -p "$TOOLS_BUILD"
VP_XML="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}/protocol/wlr-virtual-pointer-unstable-v1.xml"
VK_XML="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}/protocol/virtual-keyboard-unstable-v1.xml"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"
VI="$TOOLS_BUILD/virtual-input-test"

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-framehit.XXXXXX")"
chmod 700 "$R"
mkdir -p "$R/run"
export XDG_RUNTIME_DIR="$R/run"
SOCK="sde-framehit-$$"

# Deterministic geometry. [Placement] sets the window's *content* rect; the frame
# (titlebar) extends frame_top (20px) above it, so:
#
#   L: content 200,150 400x300  -> frame ~x200..600, y130..470, titlebar y130..152
#   O: content 300,250 400x300  -> frame ~x300..700, y230..570, titlebar y230..252
#
#   (250,350): inside L only            -- used to raise L
#   (450,240): inside O's titlebar band and inside L's frame -- the case under test
cat > "$R/WindowServer.ini" <<'EOF'
[Placement]
Windows=SDE FramePress L:200,150,400,300; SDE FramePress O:300,250,400,300
EOF

COMP_PID=""
A_PID=""
B_PID=""
cleanup() {
    # Stop the clients and the compositor *and reap them* before removing the
    # state directory. Without the wait, a compositor that is still writing its
    # state can race `rm -rf`; on CI that surfaced as "Directory not empty" and
    # made a passing test exit non-zero.
    for pid in "$A_PID" "$B_PID" "$COMP_PID"; do
        [[ -n "$pid" ]] || continue
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    # Even after reaping, a child may have handed off a last state write. Retry
    # briefly rather than turning a successful assertion run into a failure in
    # the EXIT trap.
    for _ in $(seq 1 20); do
        rm -rf "$R" 2>/dev/null && return 0
        sleep 0.05
    done
    rm -rf "$R" 2>/dev/null || true
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY -u SDE_XWAYLAND \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$R" SDE_WM_PORTAL_PATH="$R/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done

wait_for_map() {
    for _ in $(seq 1 60); do grep -aqF "toplevel mapped title='$1'" "$R/comp.log" && return 0; sleep 0.1; done
    return 1
}
# --server-side-decorations is required: the frame hit test only applies to windows
# the compositor decorates (an undecorated client moves itself).
map_window() {
    env -u WAYLAND_DISPLAY WAYLAND_DISPLAY="$SOCK" \
        "$NATIVE" --title "$1" --server-side-decorations --linger 15000 >/dev/null 2>&1 &
    echo $!
}

A_PID="$(map_window 'SDE FramePress L')"
wait_for_map 'SDE FramePress L' || { echo "  FAIL: window L did not map"; exit 1; }
sleep 0.8
B_PID="$(map_window 'SDE FramePress O')"
wait_for_map 'SDE FramePress O' || { echo "  FAIL: window O did not map"; exit 1; }
# Let the launch/focus animations settle before measuring focus by click.
sleep 1.5

click() { # click <x> <y>
    env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
        --move "$1" "$2" --sleep 80 \
        --button 0x110 press --sleep 100 --button 0x110 release --linger 250 \
        >/dev/null 2>&1 || true
    sleep 0.6
}

# O mapped last, so O is active and later in the array. Raise L by clicking a part
# of it that O does not cover: L becomes the topmost window, and stays *earlier* in
# the array -- exactly the state the bug needed.
click 250 350

fail=0
check() { # check <pattern> <description>
    if grep -aqF "$1" "$R/comp.log"; then echo "  ok: $2"; else echo "  FAIL: $2"; fail=1; fi
}
refute() { # refute <pattern> <description>
    if grep -aqF "$1" "$R/comp.log"; then echo "  FAIL: $2"; fail=1; else echo "  ok: $2"; fi
}

check  "window 'SDE FramePress O' deactivated" "clicking L's body focused L (O was deactivated)"
raise_after="$(grep -ac 'deactivated' "$R/comp.log" || true)"
if [[ "$raise_after" -eq 2 ]]; then
    echo "  ok: exactly one focus change from raising L (2 deactivations: O's map, then O)"
else
    echo "  FAIL: expected 2 deactivations after raising L, saw $raise_after"; fail=1
fi

# Everything logged from here on comes from the presses under test.
#
# The press point must fall in the occluded window's *titlebar/border band*, which
# for these test clients is only a few pixels tall (they are not themed windows), so
# probe a small range of y rather than one guessed pixel: any one of them landing in
# the band is the bug. Measured against the unfixed compositor, y=252 at x=450 is in
# that band.
mark="$(wc -l < "$R/comp.log")"
for y in 249 250 251 252 253 254; do
    env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
        --move 450 "$y" --sleep 60 \
        --button 0x110 press --sleep 80 --button 0x110 release --linger 150 \
        >/dev/null 2>&1 || true
    sleep 0.35
done

press_after="$(grep -ac 'deactivated' "$R/comp.log" || true)"
if [[ "$press_after" -eq "$raise_after" ]]; then
    echo "  ok: the presses did not move focus (still $press_after deactivation(s))"
else
    echo "  FAIL: a press moved focus ($raise_after -> $press_after deactivations)"; fail=1
fi
if tail -n "+$((mark + 1))" "$R/comp.log" | grep -aqF "window 'SDE FramePress L' deactivated"; then
    echo "  FAIL: a press deactivated L, the window under the pointer"; fail=1
else
    echo "  ok: L was not deactivated by any press"
fi
if tail -n "+$((mark + 1))" "$R/comp.log" | grep -aqE "deactivated"; then
    echo "  FAIL: a press changed the active window"; fail=1
else
    echo "  ok: no window was deactivated by any press"
fi

if grep -qaE "Assertion|SIGSEGV|Segmentation fault" "$R/comp.log"; then
    echo "  FAIL: the compositor faulted"; fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> frame-hit stacking test FAILED"
    echo "--- relevant compositor log ---"
    grep -aE "toplevel mapped|activated|deactivated" "$R/comp.log" | tail -20 | sed 's/^/    /'
    exit 1
fi
echo "==> frame-hit stacking test PASSED"
