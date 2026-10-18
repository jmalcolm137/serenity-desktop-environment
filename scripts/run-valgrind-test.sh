#!/usr/bin/env bash
# ci-tier: full
# Memcheck the compositor through a complete headless start/map/shutdown.
#
# The test is deliberately short: Valgrind's value here is the teardown path
# (client resources, repeating timers, scene/renderer destruction), which the
# sanitizer runs do not exercise under a leak checker. It fails on invalid
# reads/writes and on definite leaks; still-reachable allocations are ignored.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

if ! command -v valgrind >/dev/null 2>&1; then
    echo "==> valgrind test SKIPPED (valgrind not installed)"
    exit 0
fi

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-valgrind.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/state" "$RUNTIME/session" "$RUNTIME/wm"
SOCK="sde-valgrind-$$"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM "$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 300); do kill -0 "${COMP_PID:-}" 2>/dev/null || break; sleep 0.05; done
    rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"{,.lock}
}
trap cleanup EXIT

# Start both repeating timers (heartbeat and perf) so shutdown proves they are
# stopped and released before the event loop goes away.
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 SDE_XWAYLAND=0 \
    SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SDE_PERF_STATS="$RUNTIME/perf" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    valgrind --tool=memcheck --error-exitcode=99 \
        --leak-check=full --show-leak-kinds=definite --errors-for-leak-kinds=definite \
        --num-callers=30 --log-file="$RUNTIME/valgrind.log" \
        "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 600); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
[[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] || { echo "FAIL: compositor did not start under valgrind"; tail -30 "$RUNTIME/comp.log"; exit 1; }
sleep 2

echo "=== mapping a client under valgrind ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 30 "$NATIVE" --title "Valgrind Test" --linger 300 > "$RUNTIME/native.log" 2>&1 || true
sleep 1

kill -TERM -- "-$COMP_PID" 2>/dev/null || kill -TERM "$COMP_PID" 2>/dev/null || true
for _ in $(seq 1 600); do kill -0 "$COMP_PID" 2>/dev/null || break; sleep 0.1; done
wait "$COMP_PID" 2>/dev/null || true
COMP_PID=""

fail=0
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}
check "the client mapped" "grep -q 'toplevel mapped' '$RUNTIME/comp.log'"
check "the compositor shut down cleanly" "grep -q 'WindowServer: shut down' '$RUNTIME/comp.log'"
check "memcheck reported no errors" "grep -q 'ERROR SUMMARY: 0 errors' '$RUNTIME/valgrind.log'"
check "memcheck reported no definite leaks" \
    "! grep -qE 'definitely lost: [1-9]' '$RUNTIME/valgrind.log'"
check "all heap blocks were freed or ordinary shutdown allocations remain reachable" \
    "grep -qE 'All heap blocks were freed|still reachable:' '$RUNTIME/valgrind.log'"

if [[ "$fail" -ne 0 ]]; then
    echo "--- valgrind log (tail) ---"
    tail -80 "$RUNTIME/valgrind.log" 2>/dev/null || true
    echo "--- compositor log (tail) ---"
    tail -20 "$RUNTIME/comp.log" 2>/dev/null || true
    echo "==> valgrind test FAILED"
    exit 1
fi
echo "==> valgrind test PASSED"
