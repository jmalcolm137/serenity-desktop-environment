#!/usr/bin/env bash
# Headless key-repeat regression test.
#
# wlroots 0.20 removed the compositor-side key-repeat timer:
# wlr_keyboard_set_repeat_info() only advertises the rate/delay to clients
# (wl_keyboard.repeat_info) and never synthesises repeats, so the SDE
# WindowServer owns repeat itself. This test holds a key through the wlroots
# virtual-keyboard protocol and asserts the focused Wayland client receives
# several press events for it, spaced out at the configured rate; and that
# RepeatRate=0 disables repeat.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/key-repeat-test"
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

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-repeat.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"

COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

# Start a headless compositor with the given [Keyboard] repeat settings, hold the
# key for $hold_ms, and echo "presses first last" summarised from the client.
run_case() {
    local suffix="$1" rate="$2" delay="$3" hold_ms="$4"
    local sock="sde-repeat-$suffix-$$"
    local client_log="$RUNTIME/client-$suffix.log"
    local comp_log="$RUNTIME/comp-$suffix.log"

    cat > "$RUNTIME/input.ini" <<EOF
[Keyboard]
Layouts=us
RepeatDelay=$delay
RepeatRate=$rate
EOF

    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless \
        WLR_HEADLESS_OUTPUTS=1 \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm-$suffix" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$comp_log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 80); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    sleep 0.3

    # The client maps a toplevel to get keyboard focus and records key events.
    env WAYLAND_DISPLAY="$sock" "$TOOLS_BUILD/key-repeat-test" \
        --key "$BACKSPACE" --duration "$((hold_ms + 2000))" > "$client_log" 2>&1 &
    local client_pid=$!
    sleep 1

    # Hold the key (no release) long enough for repeat to fire several times.
    # The virtual keyboard is created by this client, so warm up with a harmless
    # Shift press (ignored by the receiver) and a short pause: the focused client
    # needs one round-trip to acquire its wl_keyboard before it can receive keys,
    # otherwise the very first press races the capability change and is dropped.
    env WAYLAND_DISPLAY="$sock" "$TOOLS_BUILD/virtual-input-test" \
        --key 42 press --key 42 release --sleep 400 \
        --key "$BACKSPACE" press --sleep "$hold_ms" --key "$BACKSPACE" release \
        > "$RUNTIME/input-$suffix.log" 2>&1 || true

    wait "$client_pid" 2>/dev/null || true
    kill "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""

    cat "$client_log" >&2
    local summary
    summary="$(grep 'key-repeat-test: summary' "$client_log" || true)"
    local presses first last
    presses="$(sed -n 's/.*presses=\([0-9]*\).*/\1/p' <<<"$summary")"
    first="$(sed -n 's/.*first=\([0-9]*\)ms.*/\1/p' <<<"$summary")"
    last="$(sed -n 's/.*last=\([0-9]*\)ms.*/\1/p' <<<"$summary")"
    echo "${presses:-0} ${first:-0} ${last:-0}"
}

fail=0

echo "=== (A) a held key repeats at the configured rate ==="
read -r presses first last <<<"$(run_case fast 20 200 1500)"
echo "press events: $presses (first ${first}ms, last ${last}ms)"
if [[ "$presses" -ge 3 ]]; then
    echo "  ok: the held key repeated ($presses press events)"
else
    echo "  FAIL: the held key did not repeat (only $presses press events)"
    fail=1
fi
if [[ $((last - first)) -ge 600 ]]; then
    echo "  ok: repeats were spread over the hold ($((last - first))ms)"
else
    echo "  FAIL: repeats were not spread over the hold (first ${first}ms, last ${last}ms)"
    fail=1
fi

echo "=== (B) RepeatRate=0 disables repeat ==="
read -r presses first last <<<"$(run_case off 0 200 1500)"
echo "press events: $presses"
if [[ "$presses" -le 1 ]]; then
    echo "  ok: a held key did not repeat with RepeatRate=0"
else
    echo "  FAIL: the key repeated despite RepeatRate=0 ($presses press events)"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> key repeat test FAILED"
    exit 1
fi
echo "==> key repeat test PASSED"
