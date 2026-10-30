#!/usr/bin/env bash
# ComboBox dropdown regression test: opening a ComboBox popup (e.g. the Display
# Settings wallpaper "Mode" box) must produce a sensible popup, not a collapsed
# (1-2px) window — including when the window is near the bottom of the screen
# (the popup must flip upward, not clamp). Asserts the popup layer-surface size.
#
# Runs entirely headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TEST="$SDE_ROOT/build/test/libwm-combo-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tests.sh" libwm-combo-test >/dev/null 2>&1 || true

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-combo.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
# Keep the whole run hermetic: applications must not reach a live session's
# portals, and the compositor sockets live under this directory.
export XDG_RUNTIME_DIR="$RUNTIME/run"
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$XDG_RUNTIME_DIR" "$SDE_SESSION_DIR/portal"
chmod 700 "$RUNTIME"
COMP_PID=""
COMP2_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP2_PID" ]] && kill "$COMP2_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

SOCK="sde-combo-$$"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    SDE_SESSION_DIR="$SDE_SESSION_DIR" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
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
        XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
        SDE_SESSION_DIR="$SDE_SESSION_DIR" \
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

# A client may call set_rect() after the compositor has placed its window. That
# must not replace the compositor-owned origin, or screen-relative coordinates
# (and this popup) fall back to window-local ones. Place the window explicitly,
# then have the payload call set_rect(200,100) and open the combo: the popup must
# still be next to the combo at the placed origin, not at (220,143).
echo "=== a later client set_rect does not move the popup ==="
RUNTIME2="$RUNTIME/reposition"
mkdir -p "$RUNTIME2"
cat > "$RUNTIME2/WindowServer.ini" <<'INI'
[Placement]
Windows=ComboBox Test:600,100,320,180
INI
SOCK2="sde-combo-move-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    SDE_SESSION_DIR="$SDE_SESSION_DIR" \
    SDE_STATE_DIR="$RUNTIME2" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm2" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK2" > "$RUNTIME2/comp.log" 2>&1 &
COMP2_PID=$!
for _ in $(seq 1 50); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK2" ]] && break
    sleep 0.1
done
sleep 1
env WAYLAND_DISPLAY="$SOCK2" \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    SDE_SESSION_DIR="$SDE_SESSION_DIR" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 15 "$TEST" 200 100 reposition > "$RUNTIME2/app.log" 2>&1 || true
POPUP2="$(grep -m1 -E "popup window [0-9]+ \([0-9]+x[0-9]+\) at [0-9]+,[0-9]+" "$RUNTIME2/app.log" || true)"
COMBO2="$(grep -m1 "COMBO-TEST: opening popup at" "$RUNTIME2/app.log" || true)"
echo "  $COMBO2"
echo "  $POPUP2"
if [[ "$COMBO2" =~ \[([0-9]+),([0-9]+)\ [0-9]+x([0-9]+)\] ]]; then
    combo_x="${BASH_REMATCH[1]}"
    combo_y="${BASH_REMATCH[2]}"
    combo_h="${BASH_REMATCH[3]}"
    expected="$combo_x,$((combo_y + combo_h - 1))"
    if [[ "$combo_x" -ge 600 && "$POPUP2" == *" at $expected" ]]; then
        echo "  ok (reposition): popup stayed at the compositor-placed origin"
    else
        echo "  FAIL (reposition): popup did not stay at the compositor-placed origin (expected $expected)"
        fail=1
    fi
else
    echo "  FAIL (reposition): could not read the combo's screen rect"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> combo-box test FAILED"
    exit 1
fi
echo "==> combo-box test PASSED"
