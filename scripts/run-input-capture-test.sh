#!/usr/bin/env bash
# Headless InputCapture test (docs/17-input-capture.md).
#
# P1: the private serenity_input_capture protocol hands a session's EIS file
#     descriptor to a client, and a libei receiver context connects to it.
# P2: the compositor reports zones (the outputs), accepts pointer barriers
#     (rejecting invalid ones), arms capture, and — when the pointer crosses a
#     barrier — emits `activated` and delivers pointer motion over EIS.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init input-capture
sde_test_export_roots
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/input-capture-test"
XML="$SDE_ROOT/Services/WindowServer/protocol/serenity-input-capture.xml"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
wayland-scanner client-header "$XML" "$TOOLS_BUILD/serenity-input-capture-client-protocol.h"
wayland-scanner private-code  "$XML" "$TOOLS_BUILD/serenity-input-capture-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/input-capture-test.c" \
    "$TOOLS_BUILD/serenity-input-capture-protocol.c" \
    $(pkg-config --cflags --libs wayland-client libei-1.0) \
    -o "$TOOLS_BUILD/input-capture-test"

VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
CLIENT_PID=""
cleanup() {
    [[ -n "$CLIENT_PID" ]] && kill "$CLIENT_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    # D8 (docs/03): unlink the compositor socket(s) this run created.
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
}

sde_test_compositor SDE_INPUT_CAPTURE=1 SDE_WM_PORTAL_PATH="$RUNTIME/wm"

# The client sets up a session, connects the libei receiver, requests zones,
# installs a barrier (plus a deliberately invalid one) and arms capture.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/input-capture-test" > "$RUNTIME/test.log" 2>&1 &
CLIENT_PID=$!
sleep 2

# Move the pointer across the barrier (the client places it at the zone's
# horizontal midpoint, x=640): 500 -> 800 crosses it. This activates capture.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --move 500 360 --move 800 360 \
    > "$RUNTIME/input.log" 2>&1 || true
sleep 0.4

# Type a key (evdev 30 = KEY_A); with keyboard capture active it must reach the
# EIS client rather than being delivered normally.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --key 30 press --key 30 release \
    > "$RUNTIME/key.log" 2>&1 || true

wait "$CLIENT_PID" 2>/dev/null || true
CLIENT_PID=""
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
cat "$RUNTIME/comp.log"
echo "--- input-capture client ---"
cat "$RUNTIME/test.log"

fail=0
check_comp() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}
check_client() {
    if grep -qF "$1" "$RUNTIME/test.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

echo "=== InputCapture (EIS fd handoff + pointer activation) ==="
# P1
check_comp "InputCapture: EIS implementation ready"
check_comp "InputCapture: handed an EIS fd to app 'input-capture-test'"
check_client "received EIS fd"
check_client "EIS connected"
# P2
check_comp "InputCapture: EIS pointer device ready"
check_client "zone 0: 1280x720 at 0,0"
check_client "1 barrier(s) rejected 2"
check_comp "has 1 barrier(s), 1 rejected"
check_client "activated activation_id="
check_client "motion dx="
# P3
check_comp "InputCapture: EIS keyboard device ready"
check_client "keyboard key code=30 press"
check_client "summary seat=1 zones=1 failed_barriers=1 activated=1 motion=1 key=1"

if [[ "$fail" -ne 0 ]]; then
    echo "==> InputCapture test FAILED"
    exit 1
fi
echo "==> InputCapture test PASSED"
