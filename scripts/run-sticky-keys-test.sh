#!/usr/bin/env bash
# ci-tier: fast
# Sticky keys (R5 accessibility, docs/29-accessibility.md).
#
# [Accessibility] StickyKeys makes a modifier tapped on its own latch, so the next
# key press is delivered with it: Shift then 'a' must reach the client as 'A'. Wayland
# clients translate keycodes themselves, so what matters is the modifier mask they are
# told about -- the test reads it back through a real client.
#
# Runs entirely headless, and asserts the *control* too: with the option off, tapping
# Shift must not affect the next key.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/sticky-keys-test"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

SHIFT=42 # evdev KEY_LEFTSHIFT
A=30     # evdev KEY_A
B=48     # evdev KEY_B

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/sticky-keys-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/sticky-keys-test"
TOOL="$TOOLS_BUILD/sticky-keys-test"

# One run: a compositor with StickyKeys=$1 and one client, then tap Shift, then 'a',
# then 'b'. $2 is where the client's key log goes.
run_case() {
    local sticky="$1" out="$2"
    local runtime sock comp_pid="" client_pid=""
    runtime="$(mktemp -d "${TMPDIR:-/tmp}/sde-sticky.XXXXXX")"
    chmod 700 "$runtime"
    mkdir -p "$runtime/run"
    {
        echo "[Accessibility]"
        echo "StickyKeys=$sticky"
        echo "[Placement]"
        echo "Windows=SDE Sticky Keys Test:0,0,320,200"
    } > "$runtime/WindowServer.ini"

    sock="sde-sticky-$$-$sticky"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        XDG_RUNTIME_DIR="$runtime/run" SDE_STATE_DIR="$runtime" SDE_XWAYLAND=0 \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$runtime/comp.log" 2>&1 &
    comp_pid=$!
    for _ in $(seq 1 80); do
        [[ -e "$runtime/run/$sock" ]] && break
        sleep 0.1
    done
    sleep 0.5

    env WAYLAND_DISPLAY="$sock" XDG_RUNTIME_DIR="$runtime/run" \
        "$TOOL" --duration 8000 > "$out" 2>&1 &
    client_pid=$!
    sleep 2.5

    # Click to focus (keys only go to the focused surface), tap Shift on its own,
    # then press 'a' and 'b'.
    env WAYLAND_DISPLAY="$sock" XDG_RUNTIME_DIR="$runtime/run" \
        "$VI" --extent 1280 720 \
        --move 100 100 --sleep 100 --button 0x110 press --sleep 80 --button 0x110 release --sleep 300 \
        --key $SHIFT press --sleep 60 --key $SHIFT release --sleep 250 \
        --key $A press --sleep 60 --key $A release --sleep 250 \
        --key $B press --sleep 60 --key $B release --linger 400 >/dev/null 2>&1 || true

    sleep 0.6
    kill "$client_pid" 2>/dev/null || true
    kill -TERM -- "-$comp_pid" 2>/dev/null || true
    wait "$comp_pid" 2>/dev/null || true
    cp "$runtime/comp.log" "${out}.comp"
    rm -rf "$runtime"
}

OFF_LOG="$(mktemp "${TMPDIR:-/tmp}/sde-sticky-off.XXXXXX")"
ON_LOG="$(mktemp "${TMPDIR:-/tmp}/sde-sticky-on.XXXXXX")"
run_case false "$OFF_LOG"
run_case true "$ON_LOG"

# The depressed mask the client was told about for a key press.
depressed_for() {
    local log="$1" key="$2"
    grep -a "sticky-keys-test: key=$key press" "$log" | head -1 | sed 's/.*depressed=\([0-9]*\).*/\1/'
}

OFF_A="$(depressed_for "$OFF_LOG" "$A")"
ON_A="$(depressed_for "$ON_LOG" "$A")"
ON_B="$(depressed_for "$ON_LOG" "$B")"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== sticky keys: off (control) ==="
grep -aE "sticky-keys-test: key=(30|48) press" "$OFF_LOG" || true
check "with StickyKeys off, tapping Shift does not affect the next key" "[[ '${OFF_A:-unset}' == 0 ]]"

echo "=== sticky keys: on ==="
grep -aE "sticky keys|sticky-keys-test: key=(30|48) press" "$ON_LOG.comp" "$ON_LOG" 2>/dev/null | head -6 || true
check "the modifier is latched and applied" "grep -q 'sticky keys applied' '$ON_LOG.comp'"
check "the next key after the tap carries the modifier" "[[ '${ON_A:-unset}' != 0 && '${ON_A:-unset}' != unset ]]"
check "the latch is used up by that one key" "[[ '${ON_B:-unset}' == 0 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- sticky-on client log ---"; tail -20 "$ON_LOG"
    echo "--- sticky-on compositor log (tail) ---"; tail -20 "$ON_LOG.comp"
    echo "==> sticky-keys test FAILED"
    exit 1
fi
echo "==> sticky-keys test PASSED"
