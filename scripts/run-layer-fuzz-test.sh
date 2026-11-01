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
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init layer-fuzz
sde_test_export_roots
FUZZ="$SDE_ROOT/build/tools/layer-fuzz-test"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
SEED="${SDE_FUZZ_SEED:-1}"
CASES="${SDE_FUZZ_CASES:-500}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$FUZZ" ]] || { echo "SKIP: layer-fuzz-test not built"; exit 0; }
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

sde_test_compositor SDE_XWAYLAND=0 SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat"
sleep 0.3

echo "=== sending $CASES layer-shell operations (seed $SEED) ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_FUZZ_SEED="$SEED" SDE_FUZZ_CASES="$CASES" \
    timeout 30 "$FUZZ" > "$RUNTIME/fuzz.log" 2>&1 || true
cat "$RUNTIME/fuzz.log"

echo "=== layer-shell fuzz ==="
sde_test_expect "the compositor is still running" kill -0 "$COMP_PID"
sde_test_refute "$RUNTIME/comp.log" "VERIFICATION FAILED|crashed with|assertion|SIGSEGV|AddressSanitizer" "no crash or assertion in the log"

# And it can still serve a fresh client, not just stay alive.
before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 5 "$NATIVE" --title "Layer Fuzz After" > "$RUNTIME/after.log" 2>&1 || true
mapped_more() { [[ "$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)" -gt "${before:-0}" ]]; }
for _ in $(seq 1 50); do
    mapped_more && break
    sleep 0.1
done
sde_test_expect "a client still maps after the fuzz stream" mapped_more

sde_test_finish
