#!/usr/bin/env bash
# Headless window-stack transition test (R3).
#
# SerenityOS slides the outgoing window stack out and the incoming one in when the
# stack (workspace) changes. SDE offsets the toplevels of both stacks for the
# duration and hides the outgoing one when it ends, gated on [Effects]
# AnimateWindows.
#
# This runs the compositor headless with a window on workspace 0,0, switches to
# 0,1, and asserts the transition was started and that the window left the screen.
# A second run with AnimateWindows=false asserts the transition does not run.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"
SHOT="$SDE_ROOT/build/tools/sde-screenshot"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if ! command -v compare >/dev/null 2>&1; then
    echo "==> stack-transition test SKIPPED (ImageMagick compare not found)"
    exit 0
fi

WIDTH="${SDE_STACK_TRANSITION_WIDTH:-800}"
HEIGHT="${SDE_STACK_TRANSITION_HEIGHT:-600}"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-transition.XXXXXX")"
chmod 700 "$RUNTIME"
COMP_PID=""
APP_PID=""
cleanup() {
    [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
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
    if grep -qF "$1" "$file"; then echo "  UNEXPECTED: $1"; fail=1; else echo "  ok (absent): $1"; fi
}

# A window on the first workspace, then a switch to the next stack.
run_scene() {
    local log="$1" animate="$2"
    cat > "$RUNTIME/WindowServer.ini" <<EOF
[Effects]
AnimateWindows=$animate
EOF
    local sock="sde-transition-$$-$RANDOM"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        SDE_STATE_DIR="$RUNTIME" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 80); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    sleep 1

    env WAYLAND_DISPLAY="$sock" "$SDE_ROOT/build/tools/native-app-test" \
        --server-side-decorations --title "Sliding" --linger 4000 >/dev/null 2>&1 &
    APP_PID=$!
    for _ in $(seq 1 60); do
        grep -q "toplevel mapped title='Sliding'" "$log" && break
        sleep 0.1
    done
    sleep 0.5
    env WAYLAND_DISPLAY="$sock" "$SHOT" "$RUNTIME/before-$animate.ppm" >/dev/null 2>&1

    # Ctrl+Alt+Right -> the next stack. Poll for the transition, then let it finish.
    env WAYLAND_DISPLAY="$sock" "$VI" --extent "$WIDTH" "$HEIGHT" \
        --key 29 press --sleep 60 --key 56 press --sleep 60 \
        --key 106 press --sleep 60 --key 106 release --sleep 60 \
        --key 56 release --sleep 60 --key 29 release --linger 300 >/dev/null 2>&1 &
    local vi_pid=$!
    for _ in $(seq 1 100); do
        grep -q "switched to workspace 0,1" "$log" && break
        sleep 0.02
    done
    wait "$vi_pid" 2>/dev/null || true
    sleep 1
    env WAYLAND_DISPLAY="$sock" "$SHOT" "$RUNTIME/after-$animate.ppm" >/dev/null 2>&1

    kill "$APP_PID" 2>/dev/null || true
    APP_PID=""
    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    kill -TERM "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock"{,.lock}
}

echo "=== window-stack transition (AnimateWindows=true) ==="
run_scene "$RUNTIME/anim.log" true
grep "window-stack" "$RUNTIME/anim.log" || true
check "window-stack transition 0,0 -> 0,1 (delta -" "$RUNTIME/anim.log"
# The window was on the workspace we left, so it must be gone afterwards.
left="$(compare -metric AE "$RUNTIME/before-true.ppm" "$RUNTIME/after-true.ppm" null: 2>&1 | awk '{print $1}' || true)"
gone="$(awk -v d="${left:-0}" 'BEGIN { print (d > 1000) ? 1 : 0 }')"
if [[ "$gone" -eq 1 ]]; then
    echo "  ok: the window left the screen with its workspace ($left pixels changed)"
else
    echo "  MISSING: the window did not leave the screen ($left pixels changed)"
    fail=1
fi

echo "=== window-stack transition disabled (AnimateWindows=false) ==="
run_scene "$RUNTIME/noanim.log" false
check_absent "window-stack transition" "$RUNTIME/noanim.log"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -15 "$RUNTIME/anim.log"
    echo "==> stack-transition test FAILED"
    exit 1
fi
echo "==> stack-transition test PASSED"
