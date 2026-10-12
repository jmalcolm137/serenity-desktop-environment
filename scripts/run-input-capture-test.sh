#!/usr/bin/env bash
# Headless InputCapture P1 test (docs/17-input-capture.md): the private
# serenity_input_capture protocol hands a session's EIS file descriptor to a
# client, and a libei *receiver* context connects to it.
#
# Covers: the global is only offered with SDE_INPUT_CAPTURE=1; connect_to_eis
# returns a working fd; the compositor accepts the resulting EIS client
# (EI_EVENT_CONNECT). No input is captured yet (P2/P3 add zones/barriers/enable).
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/input-capture-test"
XML="$SDE_ROOT/WindowServer/protocol/serenity-input-capture.xml"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
wayland-scanner client-header "$XML" "$TOOLS_BUILD/serenity-input-capture-client-protocol.h"
wayland-scanner private-code  "$XML" "$TOOLS_BUILD/serenity-input-capture-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/input-capture-test.c" \
    "$TOOLS_BUILD/serenity-input-capture-protocol.c" \
    $(pkg-config --cflags --libs wayland-client libei-1.0) \
    -o "$TOOLS_BUILD/input-capture-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-inputcapture.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"

COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

SOCK="sde-inputcapture-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_INPUT_CAPTURE=1 \
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

env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/input-capture-test" > "$RUNTIME/test.log" 2>&1 || true

kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true
COMP_PID=""

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

echo "=== InputCapture P1 (EIS fd handoff) ==="
check_comp "InputCapture: enabled"
check_comp "InputCapture: EIS implementation ready"
check_comp "InputCapture: handed an EIS fd to app 'input-capture-test'"
check_comp "InputCapture: EIS client connected"
check_client "received EIS fd"
check_client "EIS connected"

if [[ "$fail" -ne 0 ]]; then
    echo "==> InputCapture test FAILED"
    exit 1
fi
echo "==> InputCapture test PASSED"
