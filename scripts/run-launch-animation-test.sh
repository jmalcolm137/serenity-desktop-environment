#!/usr/bin/env bash
# ci-tier: full
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
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init launch-animation
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

# Run the compositor with a client launched "from" the rect 20,30 40x30.
run_scene() {
    local name="$1" animate="$2"
    sde_test_case "$name"
    cat > "$CASE_RUNTIME/WindowServer.ini" <<EOF
[Effects]
AnimateWindows=$animate
EOF
    sde_test_case_compositor
    sleep 1
    env WAYLAND_DISPLAY="$CASE_SOCK" "$SDE_ROOT/build/tools/native-app-test" \
        --server-side-decorations --title "Launched" \
        --launch-origin 20 30 40 30 --linger 700 >/dev/null 2>&1 || true
    sleep 1
    sde_test_case_stop
}

echo "=== launch animation (AnimateWindows=true) ==="
run_scene animated true
grep "launch animation" "$CASE_RUNTIME/comp.log" || true
sde_test_check "$CASE_RUNTIME/comp.log" "launch animation 40x30\+20,30 -> " "the window animates from its launch rect"

echo "=== launch animation disabled (AnimateWindows=false) ==="
run_scene still false
sde_test_refute "$CASE_RUNTIME/comp.log" "launch animation" "no launch animation when AnimateWindows=false"

sde_test_finish "$CASE_RUNTIME/comp.log"
