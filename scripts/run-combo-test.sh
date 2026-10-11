#!/usr/bin/env bash
# ComboBox dropdown regression test: opening a ComboBox popup (e.g. the Display
# Settings wallpaper "Mode" box) must produce a sensible popup, not a collapsed
# (1-2px) window — including when the window is near the bottom of the screen
# (the popup must flip upward, not clamp). Asserts the popup layer-surface size.
#
# Runs entirely headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TEST="$BUILD/bin/libwm-combo-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
ninja -C "$BUILD" libwm-combo-test >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-combo.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-combo-$$"

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

fail=0

# run_case <label> <x> <y>: open a ComboBox at (x,y) and assert the popup is a
# real window (height and width >= 10), not collapsed.
run_case() {
    local label="$1" x="$2" y="$3"
    local log="$RUNTIME/combo-$label.log"
    env WAYLAND_DISPLAY="$SOCK" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        timeout 15 "$TEST" "$x" "$y" > "$log" 2>&1 || true

    local line
    line="$(grep -m1 -E "popup window [0-9]+ \([0-9]+x[0-9]+\)" "$log" || true)"
    if [[ -z "$line" ]]; then
        echo "  FAIL ($label): no popup window was created"
        fail=1
        return
    fi
    local size width height
    size="$(sed -E 's/.*\(([0-9]+x[0-9]+)\).*/\1/' <<<"$line")"
    width="${size%x*}"
    height="${size#*x}"
    echo "  $label: $line"
    if [[ "$width" -ge 10 && "$height" -ge 10 ]]; then
        echo "  ok ($label): popup is a real size (${width}x${height})"
    else
        echo "  FAIL ($label): popup collapsed to ${width}x${height}"
        fail=1
    fi
}

echo "=== combo-box test ==="
run_case top 200 100
run_case bottom 200 560

if ! grep -qF "layer surface namespace='serenity-popup'" "$RUNTIME/comp.log"; then
    echo "  MISSING: no serenity-popup layer surface"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> combo-box test FAILED"
    exit 1
fi
echo "==> combo-box test PASSED"
