#!/usr/bin/env bash
# Headless launch-animation test (R3).
#
# SerenityOS animates a newly launched window as an outline growing out of the
# rect the launcher handed the application (__libgui_launch_origin_rect, read by
# GUI::Window and forwarded over serenity_toplevel.set_launch_origin). The
# compositor draws it as a ChromeNode overlay, gated on [Effects] AnimateWindows.
#
# This runs the compositor headless with a client that reports a launch origin and
# asserts the compositor logged the animation from the origin to the window frame.
# A second run with AnimateWindows=false asserts it does not fire.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-launch.XXXXXX")"
chmod 700 "$RUNTIME"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
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

# Run the compositor with a client launched "from" the rect 20,30 40x30.
run_scene() {
    local log="$1" animate="$2"
    cat > "$RUNTIME/WindowServer.ini" <<EOF
[Effects]
AnimateWindows=$animate
EOF
    local sock="sde-launch-$$-$RANDOM"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_OUTPUT_SCALE=1 \
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
        --server-side-decorations --title "Launched" \
        --launch-origin 20 30 40 30 --linger 700 >/dev/null 2>&1 || true
    sleep 1

    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    kill -TERM "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock"{,.lock}
}

echo "=== launch animation (AnimateWindows=true) ==="
run_scene "$RUNTIME/anim.log" true
grep "launch animation" "$RUNTIME/anim.log" || true
check "launch animation 40x30+20,30 -> " "$RUNTIME/anim.log"

echo "=== launch animation disabled (AnimateWindows=false) ==="
run_scene "$RUNTIME/noanim.log" false
check_absent "launch animation" "$RUNTIME/noanim.log"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -20 "$RUNTIME/anim.log"
    echo "==> launch-animation test FAILED"
    exit 1
fi
echo "==> launch-animation test PASSED"
