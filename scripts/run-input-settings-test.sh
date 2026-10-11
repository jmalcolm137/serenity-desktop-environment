#!/usr/bin/env bash
# Headless test for R4.6 input settings (docs/11-r4-implementation-plan.md R4.6):
# input.ini is applied to the compositor's devices at startup and on change.
#
# Covers:
#   (A) settings from input.ini are applied when the compositor starts;
#   (B) editing input.ini live re-applies them;
#   (C) the keyboard layout is applied (xkb) too.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-input-settings.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT
export SDE_STATE_DIR="$RUNTIME"

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

cat > "$RUNTIME/input.ini" <<'EOF'
[Mouse]
Acceleration=0.5
ScrollStepSize=3
DoubleClickSpeed=400
ButtonsSwitched=true
NaturalScroll=true
LeftHanded=true
TapToClick=true
[Keyboard]
Layouts=us
RepeatDelay=500
RepeatRate=30
EOF

SOCK="sde-inp-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME" \
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
LOG="$RUNTIME/comp.log"

echo "=== (A) input.ini is applied at startup ==="
check "input accel 0.5, scroll step 3, double-click 400ms, buttons-switched on, natural-scroll on, tap on, left-handed on, layouts 'us'" "$(cat "$LOG")"
check "applied input settings (accel 0.5, natural-scroll on, buttons-switched on, left-handed on, layouts 'us')" "$(cat "$LOG")"

echo "=== (B) editing input.ini live re-applies it ==="
cat > "$RUNTIME/input.ini" <<'EOF'
[Mouse]
Acceleration=2.5
NaturalScroll=false
ButtonsSwitched=false
[Keyboard]
Layouts=de
EOF
for _ in $(seq 1 40); do
    grep -qF "applied input settings (accel 2.5, natural-scroll off, buttons-switched off" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "applied input settings (accel 2.5, natural-scroll off, buttons-switched off, left-handed off, layouts 'de')" "$(cat "$LOG")"

echo "=== (C) the keyboard layout is applied (xkb) ==="
check "applied input settings (accel 2.5, natural-scroll off, buttons-switched off, left-handed off, layouts 'de')" "$(cat "$LOG")"

echo "=== (D) the setting ranges come from the config schema ==="
cat > "$RUNTIME/WindowServer.ini" <<'EOF'
[Input]
AccelMax=5
DoubleClickMax=1500
EOF
ranges="$(env -u WAYLAND_DISPLAY -u DISPLAY LD_LIBRARY_PATH="$BUILD/lib" SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" SDE_STATE_DIR="$RUNTIME" "$COMP" --print-config 2>/dev/null)"
check "AccelMax=5" "$ranges"
check "DoubleClickMax=1500" "$ranges"
check "AccelMin=0.5" "$ranges"
check "ScrollStepMin=1" "$ranges"

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> input settings test FAILED"
    exit 1
fi
echo "==> input settings test PASSED"
