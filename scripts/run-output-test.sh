#!/usr/bin/env bash
# Headless test for R4.5 output/display settings (docs/11-r4-implementation-plan.md
# R4.5): a scripted layout change reconfigures the (headless) output.
#
# Covers:
#   (A) the compositor persists the live layout to screenlayout.ini (the Serenity
#       ScreenLayout format the port's get_screen_layout / DisplaySettings read);
#   (B) a saved layout (ScaleFactor=2) is applied when the output is created;
#   (C) editing screenlayout.ini live re-applies it to the running output;
#   (D) with two outputs, the saved positions are honoured (a startup burst must
#       not clobber the layout and leave outputs auto-placed).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-output.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

start_compositor() {
    local sock="$1"
    local outputs="${2:-1}"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS="$outputs" \
        SDE_STATE_DIR="$RUNTIME" \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm-$sock" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$RUNTIME/comp-$sock.log" 2>&1 &
    echo $!
    # Wait for the socket, created after initialize() and output creation. Outputs
    # are created when the event loop starts, and the layout is persisted coalesced
    # ~120 ms later, so settle a little longer before a test reads/edits the file.
    for _ in $(seq 1 80); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    sleep 0.6
}

stop_compositor() {
    pkill -P "$1" 2>/dev/null || true
    kill "$1" 2>/dev/null || true
    wait "$1" 2>/dev/null || true
}

echo "=== (A) the live layout is persisted ==="
COMP_PID="$(start_compositor sde-out-a)"
LOG="$RUNTIME/comp-sde-out-a.log"
check "output 'HEADLESS-1'" "$(cat "$LOG")"
if [ -f "$RUNTIME/screenlayout.ini" ]; then
    echo "  ok: screenlayout.ini written"
    layout="$(cat "$RUNTIME/screenlayout.ini")"
    check "Device=HEADLESS-1" "$layout"
    check "Width=1280" "$layout"
    check "Height=720" "$layout"
    check "ScaleFactor=1" "$layout"
else
    echo "  FAIL: screenlayout.ini not written"
    fail=1
fi
stop_compositor "$COMP_PID"

echo "=== (B) a saved layout is applied on output creation ==="
cat > "$RUNTIME/screenlayout.ini" <<'EOF'
[Screens]
MainScreen=0
[Screen0]
Mode=Device
Device=HEADLESS-1
Left=0
Top=0
Width=1280
Height=720
ScaleFactor=2
EOF
COMP_PID="$(start_compositor sde-out-b)"
LOG="$RUNTIME/comp-sde-out-b.log"
check "scaling 2x (saved layout)" "$(cat "$LOG")"
check "640x360 @2x" "$(cat "$LOG")"

echo "=== (C) editing the layout live re-applies it ==="
sed 's/ScaleFactor=2/ScaleFactor=1/' "$RUNTIME/screenlayout.ini" > "$RUNTIME/layout.new"
cat "$RUNTIME/layout.new" > "$RUNTIME/screenlayout.ini"
for _ in $(seq 1 40); do
    grep -qF "applied screen layout to 'HEADLESS-1': 1280x720 @1x" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "applied screen layout to 'HEADLESS-1': 1280x720 @1x" "$(cat "$LOG")"
stop_compositor "$COMP_PID"

echo "=== (D) multi-output placement honours the saved positions ==="
cat > "$RUNTIME/screenlayout.ini" <<'EOF'
[Screens]
MainScreen=0
[Screen0]
Mode=Device
Device=HEADLESS-1
Left=0
Top=0
Width=1280
Height=720
ScaleFactor=1
[Screen1]
Mode=Device
Device=HEADLESS-2
Left=1280
Top=0
Width=1280
Height=720
ScaleFactor=1
EOF
COMP_PID="$(start_compositor sde-out-d 2)"
LOG="$RUNTIME/comp-sde-out-d.log"
check "output 'HEADLESS-1' scale 1x (saved layout)" "$(cat "$LOG")"
check "output 'HEADLESS-2' scale 1x (saved layout)" "$(cat "$LOG")"
check "usable area for 'HEADLESS-1': 1280x720 at 0,0" "$(cat "$LOG")"
check "usable area for 'HEADLESS-2': 1280x720 at 1280,0" "$(cat "$LOG")"
# A startup burst of outputs must not clobber the saved layout: HEADLESS-1 must
# keep its saved 0,0, not be auto-placed to the right of HEADLESS-2 (2560,0).
if grep -q "at 2560,0" "$LOG"; then
    echo "  FAIL: an output was auto-placed at 2560,0 instead of its saved position"
    fail=1
else
    echo "  ok: both outputs kept their saved positions (no auto-placement)"
fi
stop_compositor "$COMP_PID"

if [[ "$fail" -ne 0 ]]; then
    echo "==> output test FAILED"
    exit 1
fi
echo "==> output test PASSED"
