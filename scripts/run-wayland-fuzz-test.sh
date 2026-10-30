#!/usr/bin/env bash
# ci-tier: full
# Structured fuzzing for the Wayland xdg-shell surface/toplevel path.
#
# wayland-fuzz-test creates a real toplevel and sends valid-but-extreme requests
# (titles, min/max sizes, states, geometry, damage, scale, transforms, move,
# resize, window menu) in a deterministic random order. A protocol error that
# disconnects the fuzzer is acceptable; a compositor crash is not. The driver also
# checks that a fresh client can still map afterwards.
#
# Runs entirely headless with a private compositor, so it is safe next to a live
# session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
FUZZ="$SDE_ROOT/build/tools/wayland-fuzz-test"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
SEED="${SDE_FUZZ_SEED:-1}"
CASES="${SDE_FUZZ_CASES:-500}"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$FUZZ" ]] || { echo "SKIP: wayland-fuzz-test not built"; exit 0; }
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-wayland-fuzz.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-wayland-fuzz-$$"
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

echo "=== sending $CASES xdg-shell operations (seed $SEED) ==="
status=0
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    SDE_FUZZ_SEED="$SEED" SDE_FUZZ_CASES="$CASES" \
    timeout 30 "$FUZZ" > "$RUNTIME/fuzz.log" 2>&1 || status=$?
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

# The fuzzer may be disconnected by a protocol error (that is the protocol's
# isolation working); a timeout or a `timeout` kill is not acceptable.
check "the fuzzer completed or was cleanly disconnected" "[[ '$status' -eq 0 ]]"
check "the compositor is still running" "kill -0 '$COMP_PID' 2>/dev/null"
check "no crash or assertion in the log" \
    "! grep -qE 'VERIFICATION FAILED|crashed with|assertion|SIGSEGV|AddressSanitizer' '$RUNTIME/comp.log'"

# And it can still serve a fresh client, not just stay alive.
before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 5 "$NATIVE" --title "Wayland Fuzz After" > "$RUNTIME/after.log" 2>&1 || true
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
    echo "==> Wayland protocol fuzz test FAILED"
    exit 1
fi
echo "==> Wayland protocol fuzz test PASSED"
