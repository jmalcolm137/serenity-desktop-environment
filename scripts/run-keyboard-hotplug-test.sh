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
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init keyboard-hotplug
sde_test_export_roots
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

chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
CLIENT_PID=""
HOLDER_PID=""
cleanup() {
    [[ -n "$HOLDER_PID" ]] && kill "$HOLDER_PID" 2>/dev/null || true
    [[ -n "$CLIENT_PID" ]] && kill "$CLIENT_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    # D8 (docs/03): unlink the compositor socket(s) this run created.
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
}

cat > "$RUNTIME/input.ini" <<'EOF'
[Keyboard]
Layouts=us
RepeatDelay=200
RepeatRate=20
EOF

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"

# The client connects before any keyboard exists, so it starts with no
# wl_keyboard (seat capability without KEYBOARD). It stays up across both
# hotplug phases; the test stops it when it is done rather than waiting out a
# fixed duration.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/key-repeat-test" \
    --key "$BACKSPACE" --duration 30000 > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
sleep 1

# Wait for a line to appear N times in the client log. Capability changes and the
# re-enter are events, not timed steps, so poll for them instead of sleeping.
wait_for_count() {
    local pattern="$1" expected="$2" count i
    for i in $(seq 1 100); do
        count="$(grep -cF "$pattern" "$RUNTIME/client.log" 2>/dev/null || true)"
        [[ "${count:-0}" -ge "$expected" ]] && return 0
        sleep 0.1
    done
    return 1
}

# Phase 1: the virtual keyboard appears; warm up with a harmless Shift (the very
# first press may race the capability change and is allowed to be lost), pause
# for the client to acquire the keyboard, then hold Backspace so repeat runs.
# --linger keeps the keyboard present for a while before the process exits, and
# its exit then removes the keyboard again.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key 42 press --key 42 release --sleep 500 \
    --key "$BACKSPACE" press --sleep 1000 --key "$BACKSPACE" release \
    --linger 800 > "$RUNTIME/input.log" 2>&1 || true
wait_for_count 'capabilities pointer=1 keyboard=0' 2 || true
wait_for_count 'keyboard leave (' 1 || true
presses_before_replug="$(grep -cF "key-repeat-test: key=$BACKSPACE press" "$RUNTIME/client.log" || true)"

# Phase 2: plug a keyboard in again. The compositor should still have the client
# focused, so the fresh wl_keyboard gets an enter and a key works without any
# pointer or window activation in between.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --linger 8000 > "$RUNTIME/holder.log" 2>&1 &
HOLDER_PID=$!
wait_for_count 'capabilities pointer=1 keyboard=1' 2 || true
wait_for_count 'keyboard enter (' 2 || true
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --key "$BACKSPACE" press --sleep 100 --key "$BACKSPACE" release \
    > "$RUNTIME/input2.log" 2>&1 || true
presses_after_replug="$(grep -cF "key-repeat-test: key=$BACKSPACE press" "$RUNTIME/client.log" || true)"
kill "$HOLDER_PID" 2>/dev/null || true
wait "$HOLDER_PID" 2>/dev/null || true
HOLDER_PID=""

kill "$CLIENT_PID" 2>/dev/null || true
wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

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
no_keyboard="$(grep -cF 'capabilities pointer=1 keyboard=0' "$RUNTIME/client.log" || true)"
enters="$(grep -cF 'keyboard enter (' "$RUNTIME/client.log" || true)"
leaves="$(grep -cF 'keyboard leave (' "$RUNTIME/client.log" || true)"

echo "=== keyboard hotplug ==="
check_contains "capabilities pointer=1 keyboard=1"   # keyboard added
if [[ "$no_keyboard" -ge 2 ]]; then
    echo "  ok: keyboard capability was withdrawn again ($no_keyboard times)"
else
    echo "  FAIL: keyboard capability was not withdrawn (saw '$no_keyboard' pointer-keyboard states)"
    fail=1
fi
if [[ "$leaves" -ge 1 ]]; then
    echo "  ok: the client got a keyboard leave when the device went away"
else
    echo "  FAIL: no keyboard leave"
    fail=1
fi
if [[ "${presses_before_replug:-0}" -ge 3 ]]; then
    echo "  ok: input and repeat work while the keyboard is present (${presses_before_replug} press events)"
else
    echo "  FAIL: no repeated input (only ${presses_before_replug:-0} press events)"
    fail=1
fi
if [[ "$enters" -ge 2 ]]; then
    echo "  ok: the client was re-entered after the keyboard was replugged ($enters enters)"
else
    echo "  FAIL: the client was not re-entered after the replug (saw $enters enters)"
    fail=1
fi
if [[ "${presses_after_replug:-0}" -gt "${presses_before_replug:-0}" ]]; then
    echo "  ok: a key works after the replug without re-activating the window"
else
    echo "  FAIL: no key after the replug (${presses_before_replug:-0} -> ${presses_after_replug:-0} press events)"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> keyboard hotplug test FAILED"
    exit 1
fi
echo "==> keyboard hotplug test PASSED"
