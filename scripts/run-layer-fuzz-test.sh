#!/usr/bin/env bash
# ci-tier: full
# Structured fuzzing for the layer-shell arrange path.
#
# layer-fuzz-test creates one layer surface and drives anchors, exclusive zones,
# margins, desired sizes, layers and keyboard interactivity with valid-but-extreme
# values. The compositor must arrange or reject them and keep serving; the driver
# also maps a fresh client afterwards.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
FUZZ="$SDE_ROOT/build/tools/layer-fuzz-test"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
SEED="${SDE_FUZZ_SEED:-1}"
CASES="${SDE_FUZZ_CASES:-500}"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$FUZZ" ]] || { echo "SKIP: layer-fuzz-test not built"; exit 0; }
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-layer-fuzz.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-layer-fuzz-$$"
COMP_PID=""
cleanup() {
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
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.3

echo "=== sending $CASES layer-shell operations (seed $SEED) ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    SDE_FUZZ_SEED="$SEED" SDE_FUZZ_CASES="$CASES" \
    timeout 30 "$FUZZ" > "$RUNTIME/fuzz.log" 2>&1 || true
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
    "! grep -qE 'VERIFICATION FAILED|crashed with|assertion|SIGSEGV|AddressSanitizer' '$RUNTIME/comp.log'"

before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 5 "$NATIVE" --title "Layer Fuzz After" > "$RUNTIME/after.log" 2>&1 || true
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
    echo "==> layer-shell fuzz test FAILED"
    exit 1
fi
echo "==> layer-shell fuzz test PASSED"
