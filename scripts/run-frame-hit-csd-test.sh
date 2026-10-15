#!/usr/bin/env bash
# Frame-press regression: a client-decorated window owns presses on its own titlebar.
#
# hit_test_frame() walks the windows topmost-first and, for the first one whose frame
# rectangle contains the point, returns that window's frame region. A
# client-side-decorated window has no compositor frame to return -- but the search
# used to *continue* past it, so a press on its titlebar fell through to a window
# behind it. That is the reported "clicking Firefox's titlebar to maximise it
# maximises the Terminal behind it": Firefox draws its own titlebar, the Terminal
# underneath has a compositor frame whose rectangle covers the same point, and the
# press was attributed to the Terminal.
#
# The sibling test (run-frame-hit-stacking-test.sh) uses two compositor-decorated
# windows and covers the *ordering* half. It passed while this bug was live, which is
# why this case exists separately.
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

mkdir -p "$TOOLS_BUILD"
VP_XML="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}/protocol/wlr-virtual-pointer-unstable-v1.xml"
VK_XML="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}/protocol/virtual-keyboard-unstable-v1.xml"
[ -x "$TOOLS_BUILD/virtual-input-test" ] || {
    wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
    wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
    wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
    wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
    cc -O2 -I "$TOOLS_BUILD" \
        "$SDE_ROOT/tools/virtual-input-test.c" \
        "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
        "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
        $(pkg-config --cflags --libs wayland-client xkbcommon) \
        -o "$TOOLS_BUILD/virtual-input-test"
}
VI="$TOOLS_BUILD/virtual-input-test"

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-csdframe.XXXXXX")"
chmod 700 "$R"
mkdir -p "$R/run"
export XDG_RUNTIME_DIR="$R/run"
SOCK="sde-csdframe-$$"

# Geometry. [Placement] is the window's rect; for these unthemed clients the
# compositor-drawn titlebar band is the first couple of pixels of it, so BACK's band
# is y=250..252. FRONT covers y>=250 there, so a click at (450,250..252) is inside
# both -- on FRONT (which owns it, drawing its own titlebar) and inside BACK's
# titlebar band (so without the fix BACK takes the press).
#
#   BACK  (compositor-decorated): 200,250 400x300 -> x200..600, y250..550, band 250..252
#   FRONT (client-decorated):     300,250 400x300 -> x300..700, y250..550
cat > "$R/WindowServer.ini" <<'EOF'
[Placement]
Windows=SDE FrameHit Back:200,250,400,300; SDE FrameHit Front:300,250,400,300
EOF

COMP_PID=""
BACK_PID=""
FRONT_PID=""
cleanup() {
    [[ -n "$FRONT_PID" ]] && kill "$FRONT_PID" 2>/dev/null || true
    [[ -n "$BACK_PID" ]] && kill "$BACK_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    rm -rf "$R"
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

# BACK asks for server-side decorations; FRONT does not, so it draws its own.
env -u WAYLAND_DISPLAY WAYLAND_DISPLAY="$SOCK" \
    "$NATIVE" --title "SDE FrameHit Back" --server-side-decorations --linger 20000 >/dev/null 2>&1 &
BACK_PID=$!
wait_for_map "SDE FrameHit Back" || { echo "  FAIL: the back window did not map"; exit 1; }
sleep 0.8
env -u WAYLAND_DISPLAY WAYLAND_DISPLAY="$SOCK" \
    "$NATIVE" --title "SDE FrameHit Front" --linger 20000 >/dev/null 2>&1 &
FRONT_PID=$!
wait_for_map "SDE FrameHit Front" || { echo "  FAIL: the front window did not map"; exit 1; }
sleep 1.5

fail=0
check()  { if grep -aqF "$1" "$R/comp.log"; then echo "  ok: $2"; else echo "  FAIL: $2"; fail=1; fi; }
refute() { if grep -aqF "$1" "$R/comp.log"; then echo "  FAIL: $2"; fail=1; else echo "  ok: $2"; fi; }

# FRONT mapped last, so it is on top and active; nothing else may take its presses.
check "toplevel mapped title='SDE FrameHit Front'" "both windows mapped"
mark="$(wc -l < "$R/comp.log")"

# Press on FRONT's own titlebar, at the pixels that also sit in BACK's titlebar
# band (only those discriminate; points outside FRONT belong to BACK and would
# legitimately move the focus).
for y in 250 251 252; do
    env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
        --move 450 "$y" --sleep 60 \
        --button 0x110 press --sleep 80 --button 0x110 release --linger 150 \
        >/dev/null 2>&1 || true
    sleep 0.35
done

# Focusing the front window also deactivates the back one (the compositor keeps
# exactly one window active), so the discriminating question is whether FRONT was
# deactivated -- that only happens if a press was taken by the window behind it.
if tail -n "+$((mark + 1))" "$R/comp.log" | grep -aqF "window 'SDE FrameHit Front' deactivated"; then
    echo "  FAIL: a press reached BACK, behind the front window"; fail=1
else
    echo "  ok: no press reached the window behind (FRONT kept its presses)"
fi
if tail -n "+$((mark + 1))" "$R/comp.log" | grep -aqE "WindowServer: tiled|interactive"; then
    echo "  FAIL: a press started a move/maximise"; fail=1
else
    echo "  ok: no move/maximise was started"
fi

if grep -qaE "Assertion|SIGSEGV|Segmentation fault" "$R/comp.log"; then
    echo "  FAIL: the compositor faulted"; fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> frame-hit CSD test FAILED"
    echo "--- relevant compositor log ---"
    grep -aE "toplevel mapped|activated|deactivated|tiled|frame" "$R/comp.log" | tail -20 | sed 's/^/    /'
    exit 1
fi
echo "==> frame-hit CSD test PASSED"
