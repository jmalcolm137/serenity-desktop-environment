#!/usr/bin/env bash
# Headless applet-area test (docs/05-r3-plan.md §4 WS4): create several applet
# layer surfaces ("serenity-applet:<name>") and assert the compositor lays them
# out in [Applet] Order, right-to-left, with the Taskbar-supplied position (here
# the fallback bottom-right of the usable area, since no Taskbar runs).
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/applet-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
LAYER_XML="$WLROOTS_SRC/protocol/wlr-layer-shell-unstable-v1.xml"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$LAYER_XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$LAYER_XML" "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/applet-test.c" \
    "$TOOLS_BUILD/wlr-layer-shell-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/applet-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-applet.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

SOCK="sde-applet-$$"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 1

# Three applets. CPUGraph is in [Applet] Order (index 1); Alpha/Beta are not
# (index 0), so CPUGraph sorts first and, laid right-to-left, ends up rightmost.
env WAYLAND_DISPLAY="$SOCK" setsid timeout 15 "$TOOLS_BUILD/applet-test" \
    --namespace serenity-applet:Alpha --width 40 --height 21 > "$RUNTIME/alpha.log" 2>&1 &
sleep 0.5
env WAYLAND_DISPLAY="$SOCK" setsid timeout 15 "$TOOLS_BUILD/applet-test" \
    --namespace serenity-applet:Beta --width 30 --height 21 > "$RUNTIME/beta.log" 2>&1 &
sleep 0.5
env WAYLAND_DISPLAY="$SOCK" setsid timeout 15 "$TOOLS_BUILD/applet-test" \
    --namespace serenity-applet:CPUGraph --width 20 --height 21 > "$RUNTIME/cpu.log" 2>&1 &
sleep 1.5

# A live resolution change must move the applet area with the output. Without the
# output commit re-arranging the layers, the compositor keeps the old usable area
# (and the old 1280x720 coordinates), which is how the applet strip ended up
# floating away from the Taskbar after a Display Settings change.
echo "=== live resolution change ==="
cat > "$RUNTIME/screenlayout.ini" <<'INI'
[Screens]
MainScreen=0

[Screen0]
Mode=Device
Device=HEADLESS-1
Left=0
Top=0
Width=800
Height=600
ScaleFactor=1
INI
for _ in $(seq 1 40); do
    grep -q "usable area for 'HEADLESS-1': 800x600 at 0,0" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.3

# A live output-scale change shrinks the logical output again (800x600 @2x is
# 400x300 logical); the area must follow.
echo "=== live output scale change ==="
sed 's/ScaleFactor=1/ScaleFactor=2/' "$RUNTIME/screenlayout.ini" > "$RUNTIME/layout.new"
cat "$RUNTIME/layout.new" > "$RUNTIME/screenlayout.ini"
for _ in $(seq 1 40); do
    grep -q "usable area for 'HEADLESS-1': 400x300 at 0,0" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
sleep 0.3

kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
cat "$RUNTIME/comp.log"

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

echo "=== applet-area test ==="
# The applet-area size is the sum of the widths (40+30+20) plus three spacings
# (4 each), and it is told to the Taskbar for sizing. The order string proves
# [Applet] Order was honoured.
check "AppletManager: applet area size 98x19 (3 applets: CPUGraph,Alpha,Beta,)"
# Right-to-left: CPUGraph rightmost (right edge - 20), then Alpha, then Beta.
check "'CPUGraph' 20x21+1260,700"
check "'Alpha' 40x21+1216,700"
check "'Beta' 30x21+1182,700"

echo "=== the area follows a resolution change ==="
check "usable area for 'HEADLESS-1': 800x600 at 0,0"
check "'CPUGraph' 20x21+780,580"
check "'Alpha' 40x21+736,580"
check "'Beta' 30x21+702,580"

echo "=== the area follows an output-scale change ==="
check "usable area for 'HEADLESS-1': 400x300 at 0,0"
check "'CPUGraph' 20x21+380,280"
check "'Alpha' 40x21+336,280"
check "'Beta' 30x21+302,280"

if [[ "$fail" -ne 0 ]]; then
    echo "==> applet-area test FAILED"
    exit 1
fi
echo "==> applet-area test PASSED"
