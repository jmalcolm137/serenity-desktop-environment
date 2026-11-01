#!/usr/bin/env bash
# ci-tier: full
# Structured fuzzing for the data-device selection/drag setup path.
#
# data-device-fuzz-test creates sources with random MIME offers/actions, sets
# selections and starts drags with/without icons under random serials. The
# compositor must clean up sources without crashing; a protocol error for an
# invalid serial is allowed, and a fresh client must still map afterwards.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init data-device-fuzz
sde_test_export_roots
FUZZ="$SDE_ROOT/build/tools/data-device-fuzz-test"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
SEED="${SDE_FUZZ_SEED:-1}"
CASES="${SDE_FUZZ_CASES:-500}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$FUZZ" ]] || { echo "SKIP: data-device-fuzz-test not built"; exit 0; }
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

sde_test_compositor SDE_XWAYLAND=0 SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat"
sleep 0.3

echo "=== sending $CASES data-device operations (seed $SEED) ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_FUZZ_SEED="$SEED" SDE_FUZZ_CASES="$CASES" \
    timeout 30 "$FUZZ" > "$RUNTIME/fuzz.log" 2>&1 || true
cat "$RUNTIME/fuzz.log"

echo "=== data-device fuzz ==="
sde_test_expect "the compositor is still running" kill -0 "$COMP_PID"
sde_test_refute "$RUNTIME/comp.log" "VERIFICATION FAILED|crashed with|assertion|SIGSEGV|AddressSanitizer" "no crash or assertion in the log"

# And it can still serve a fresh client, not just stay alive.
before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 5 "$NATIVE" --title "Data Fuzz After" > "$RUNTIME/after.log" 2>&1 || true
mapped_more() { [[ "$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)" -gt "${before:-0}" ]]; }
for _ in $(seq 1 50); do
    mapped_more && break
    sleep 0.1
done
sde_test_expect "a client still maps after the fuzz stream" mapped_more

sde_test_finish
