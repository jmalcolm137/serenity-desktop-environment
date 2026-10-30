#!/usr/bin/env bash
# ci-tier: full
# Structured fuzzing for the WindowManagerServer protocol.
#
# The Taskbar, WorkspacePicker and applets drive the shell over this IPC protocol.
# libwm-wm-fuzz-test connects as one of them and sends boundary/random values for
# every request (ids, rects, workspaces, resize directions, applet positions,
# manager ids, event masks, keymaps). The compositor must reject or clamp them and
# keep serving; the driver checks it is alive and can still map a client.
#
# Runs entirely headless with a private WM portal, so it is safe next to a live
# session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
FUZZ="$SDE_ROOT/build/test/libwm-wm-fuzz-test"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
SEED="${SDE_FUZZ_SEED:-1}"
CASES="${SDE_FUZZ_CASES:-500}"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
"$SDE_ROOT/scripts/build-tests.sh" libwm-wm-fuzz-test >/dev/null 2>&1 || true
[[ -x "$FUZZ" ]] || { echo "SKIP: $FUZZ not built"; exit 0; }
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-wm-fuzz.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-wm-fuzz-$$"
COMP_PID=""
APP_PID=""
cleanup() {
    [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM "$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-}" 2>/dev/null || break; sleep 0.05; done
    rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"{,.lock}
}
trap cleanup EXIT

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 SDE_XWAYLAND=0 \
    SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" && -S "$RUNTIME/wm" ]] && break
    sleep 0.1
done
sleep 0.3

# A real window: wm_id 1 is the target the fuzzer also pokes with valid-looking
# ids and extreme rects.
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$NATIVE" --title "WM Fuzz Target" --linger 20000 > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!
for _ in $(seq 1 100); do
    grep -q "toplevel mapped" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done

echo "=== sending $CASES WindowManagerServer requests (seed $SEED) ==="
env SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SDE_FUZZ_SEED="$SEED" SDE_FUZZ_CASES="$CASES" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$FUZZ" > "$RUNTIME/fuzz.log" 2>&1 || {
    echo "  FAIL: the fuzzer itself failed"
    tail -20 "$RUNTIME/fuzz.log"
    exit 1
}
cat "$RUNTIME/fuzz.log"

fail=0
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}

check "the compositor is still running" "kill -0 '$COMP_PID' 2>/dev/null"
check "no crash or assertion in the log" \
    "! grep -qE 'VERIFICATION FAILED|crashed with|assertion|SIGSEGV' '$RUNTIME/comp.log'"

# And it can still serve a fresh client, not just stay alive.
before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 5 "$NATIVE" --title "WM Fuzz After" > "$RUNTIME/after.log" 2>&1 || true
for _ in $(seq 1 50); do
    after="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
    [[ "${after:-0}" -gt "${before:-0}" ]] && break
    sleep 0.1
done
check "a client still maps after the fuzz stream" \
    "[[ \"\$(grep -c 'toplevel mapped' '$RUNTIME/comp.log' || true)\" -gt '${before:-0}' ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"
    tail -40 "$RUNTIME/comp.log" 2>/dev/null || true
    echo "==> WindowManagerServer fuzz test FAILED"
    exit 1
fi
echo "==> WindowManagerServer fuzz test PASSED"
