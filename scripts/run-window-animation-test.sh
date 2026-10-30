#!/usr/bin/env bash
# Headless window-animation test.
#
# SerenityOS animates minimize/restore as an outline between the window and its
# Taskbar button. The compositor draws it as a ChromeNode overlay, using the
# button rect the Taskbar reports over the WindowManagerServer
# (set_window_taskbar_rect), and gates it on [Effects] AnimateWindows.
#
# This runs the compositor headless with the Taskbar and a placed Text Editor,
# minimizes the window (Super+Down), then restores it by clicking the Taskbar
# button, and asserts the compositor logged both animations (with the button
# rect as the target, proving set_window_taskbar_rect is consumed). A second run
# with AnimateWindows=false asserts neither animation fires.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-anim.XXXXXX")"
chmod 700 "$RUNTIME"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

fail=0
check() {
    local file="${2:-$RUNTIME/comp.log}"
    if grep -qF "$1" "$file"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi
}
check_absent() {
    local file="${2:-$RUNTIME/comp.log}"
    if grep -qF "$1" "$file"; then
        echo "  UNEXPECTED: $1"
        # Say which line; a bare "animation" match is otherwise a mystery.
        grep -F "$1" "$file" | head -3 | sed 's/^/      /'
        fail=1
    else
        echo "  ok (absent): $1"
    fi
}

# Write the WindowServer config and run the compositor with the Taskbar + editor.
run_scene() {
    local log="$1" animate="$2"
    cat > "$RUNTIME/WindowServer.ini" <<EOF
[Placement]
Windows=Text Editor:100,100,400,300
[Effects]
AnimateWindows=$animate
EOF
    local sock="sde-anim-$$-$RANDOM"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_OUTPUT_SCALE=1 \
        SDE_STATE_DIR="$RUNTIME" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SDE_APP_BIN_DIR="$BUILD/bin" \
        "$COMP" -n "$sock" -s "$BUILD/bin/Taskbar & $BUILD/bin/TextEditor" > "$log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 60); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    SCENE_SOCK="$sock"
    sleep 6 # let the Taskbar map and report the button rect
}

stop_scene() {
    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/"$SCENE_SOCK"* 2>/dev/null || true
}

# --- Phase A: animations on (default) ----------------------------------------
run_scene "$RUNTIME/comp.log" true

# Minimize the focused window (Super+Down).
env WAYLAND_DISPLAY="$SCENE_SOCK" "$VI" --extent 1280 720 \
    --key 125 press --sleep 60 --key 108 press --sleep 60 --key 108 release --sleep 60 --key 125 release --linger 800 >/dev/null 2>&1
sleep 0.6

echo "=== minimize ==="
grep -E "animation" "$RUNTIME/comp.log" | tail -2 || true
check "WindowServer: minimize animation"

# The target is the Taskbar button rect (not the 4x4 fallback), proving the
# Taskbar's set_window_taskbar_rect was consumed.
TARGET="$(sed -nE 's/.*minimize animation [0-9]+x[0-9]+\+[0-9]+,[0-9]+ -> ([0-9]+)x([0-9]+)\+([0-9]+),([0-9]+).*/\1 \2 \3 \4/p' "$RUNTIME/comp.log" | tail -1)"
read -r TW TH TX TY <<<"${TARGET:-0 0 0 0}"
if [[ "$TW" -gt 4 && "$TH" -gt 4 ]]; then
    echo "  ok: target is the Taskbar button rect (${TW}x${TH}+${TX},${TY})"
else
    echo "  MISSING: a Taskbar button rect target (got '${TARGET:-none}')"
    fail=1
fi

# Restore by clicking the Taskbar button (its centre).
if [[ "$TW" -gt 4 ]]; then
    env WAYLAND_DISPLAY="$SCENE_SOCK" "$VI" --extent 1280 720 \
        --move $((TX + TW / 2)) $((TY + TH / 2)) --sleep 200 --button 0x110 press --sleep 120 --button 0x110 release --linger 800 >/dev/null 2>&1
    sleep 0.6
fi
echo "=== restore ==="
grep -E "animation" "$RUNTIME/comp.log" | tail -2 || true
check "WindowServer: restore animation"
stop_scene

# --- Phase B: animations off -------------------------------------------------
run_scene "$RUNTIME/off.log" false
env WAYLAND_DISPLAY="$SCENE_SOCK" "$VI" --extent 1280 720 \
    --key 125 press --sleep 60 --key 108 press --sleep 60 --key 108 release --sleep 60 --key 125 release --linger 800 >/dev/null 2>&1
sleep 0.6
echo "=== animations off ==="
check_absent "minimize animation" "$RUNTIME/off.log"
check_absent "restore animation" "$RUNTIME/off.log"
check_absent "launch animation" "$RUNTIME/off.log"
stop_scene

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -20 "$RUNTIME/comp.log"
    echo "==> window-animation test FAILED"
    exit 1
fi
echo "==> window-animation test PASSED"
