#!/usr/bin/env bash
# Headless tooltip test (docs/05-r3-plan.md §4 WS6): run a LibGUI client that asks
# for a tooltip through GUI::Application::show_tooltip, drive the pointer over the
# window, and assert the compositor creates the tooltip as an undecorated,
# non-focusable overlay layer surface ("serenity-tooltip") near the cursor -
# rather than a managed toplevel, so it stays out of the window list/switcher.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TEST="$SDE_ROOT/build/test/libwm-tooltip-test"
TOOLS_BUILD="$SDE_ROOT/build/tooltip-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tests.sh" libwm-tooltip-test >/dev/null 2>&1 || true

# The virtual pointer, to move the cursor over the window.
mkdir -p "$TOOLS_BUILD"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-tooltip.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

SOCK="sde-tooltip-$$"

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

env WAYLAND_DISPLAY="$SOCK" \
    SDE_SESSION_DIR="$SESSION_DIR" \
    SDE_APP_BIN_DIR="$BUILD/bin" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 15 "$TEST" > "$RUNTIME/client.log" 2>&1 &
TEST_PID=$!

# Put the cursor at (150,100); the tooltip should appear just below it.
sleep 0.3
env WAYLAND_DISPLAY="$SOCK" \
    "$TOOLS_BUILD/virtual-input-test" --extent 1280 720 --move 200 200 --sleep 200 \
    > "$RUNTIME/vin.log" 2>&1 || true

wait "$TEST_PID" 2>/dev/null || true
sleep 1
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- client log ---"
cat "$RUNTIME/client.log"
echo "--- compositor log ---"
cat "$RUNTIME/comp.log"

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/client.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING (client): $1"
        fail=1
    fi
    return 0
}
check_comp() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING (compositor): $1"
        fail=1
    fi
    return 0
}

echo "=== tooltip test ==="
# The client asked for a tooltip (the Tooltips system effect let it through).
check "TOOLTIP-TEST: requested"
# The tooltip is an overlay layer surface: undecorated, above normal windows, and
# because it is a layer surface it is not in the window list or the switcher.
check_comp "layer surface namespace='serenity-tooltip' layer=overlay"
# Positioned near the cursor: (150,100) + the 14px vertical offset SerenityOS uses.
check "popup window"
check "at 200,214"

# Exactly one managed toplevel (the test window) - the tooltip is not one.
toplevel_count="$(grep -cF "toplevel mapped" "$RUNTIME/comp.log" || true)"
if [[ "$toplevel_count" -eq 1 ]]; then
    echo "  ok: one managed toplevel (the test window)"
else
    echo "  MISSING: exactly one managed toplevel (count=$toplevel_count)"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> tooltip test FAILED"
    exit 1
fi
echo "==> tooltip test PASSED"
