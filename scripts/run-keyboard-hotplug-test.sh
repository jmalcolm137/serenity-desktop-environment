#!/usr/bin/env bash
# Headless keyboard-hotplug test.
#
# A client that connects while no keyboard exists has no wl_keyboard. When a
# keyboard is later hotplugged, the seat must advertise the keyboard capability,
# the client must acquire a working wl_keyboard (keymap + enter), and input
# (including repeat) must work. When the keyboard goes away again the capability
# must be withdrawn, so the seat does not keep advertising a keyboard that is not
# there (which would hand a later client a keymap-less wl_keyboard).
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/keyboard-hotplug-test"
BACKSPACE=14 # evdev KEY_BACKSPACE

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"

wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"

cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/key-repeat-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/key-repeat-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-hotplug.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"

COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    # D8 (docs/03): unlink the compositor socket(s) this run created.
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

cat > "$RUNTIME/input.ini" <<'EOF'
[Keyboard]
Layouts=us
RepeatDelay=200
RepeatRate=20
EOF

SOCK="sde-hotplug-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.3

# The client connects before any keyboard exists, so it starts with no
# wl_keyboard (seat capability without KEYBOARD).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/key-repeat-test" \
    --key "$BACKSPACE" --duration 6000 > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
sleep 1

# The virtual keyboard appears only now; warm up with a harmless Shift (the very
# first press may race the capability change and is allowed to be lost), pause
# for the client to acquire the keyboard, then hold Backspace so repeat runs.
# --linger keeps the keyboard present for a while before the process exits, and
# its exit then removes the keyboard again.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 42 press --key 42 release --sleep 500 \
    --key "$BACKSPACE" press --sleep 1000 --key "$BACKSPACE" release \
    --linger 800 > "$RUNTIME/input.log" 2>&1 || true

wait "$CLIENT_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true
COMP_PID=""

echo "--- keyboard client ---"
cat "$RUNTIME/client.log"

fail=0
check_contains() {
    if grep -qF "$1" "$RUNTIME/client.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}
summary="$(grep 'key-repeat-test: summary' "$RUNTIME/client.log" || true)"
presses="$(sed -n 's/.*presses=\([0-9]*\).*/\1/p' <<<"$summary")"
no_keyboard="$(grep -cF 'capabilities pointer=1 keyboard=0' "$RUNTIME/client.log" || true)"

echo "=== keyboard hotplug ==="
check_contains "capabilities pointer=1 keyboard=1"   # keyboard added
if [[ "$no_keyboard" -ge 2 ]]; then
    echo "  ok: keyboard capability was withdrawn again ($no_keyboard times)"
else
    echo "  FAIL: keyboard capability was not withdrawn (saw '$no_keyboard' pointer-keyboard states)"
    fail=1
fi
if [[ "${presses:-0}" -ge 3 ]]; then
    echo "  ok: input and repeat work after the hotplug ($presses press events)"
else
    echo "  FAIL: no repeated input after the hotplug (only ${presses:-0} press events)"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> keyboard hotplug test FAILED"
    exit 1
fi
echo "==> keyboard hotplug test PASSED"
