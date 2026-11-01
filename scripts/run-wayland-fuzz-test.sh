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
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init wayland-fuzz
sde_test_export_roots
FUZZ="$SDE_ROOT/build/tools/wayland-fuzz-test"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
SEED="${SDE_FUZZ_SEED:-1}"
CASES="${SDE_FUZZ_CASES:-500}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$FUZZ" ]] || { echo "SKIP: wayland-fuzz-test not built"; exit 0; }
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

sde_test_compositor SDE_XWAYLAND=0 SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat"
sleep 0.3

echo "=== sending $CASES xdg-shell operations (seed $SEED) ==="
status=0
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_FUZZ_SEED="$SEED" SDE_FUZZ_CASES="$CASES" \
    timeout 30 "$FUZZ" > "$RUNTIME/fuzz.log" 2>&1 || status=$?
cat "$RUNTIME/fuzz.log"

echo "=== Wayland protocol fuzz ==="
sde_test_expect "the compositor is still running" kill -0 "$COMP_PID"
sde_test_refute "$RUNTIME/comp.log" "VERIFICATION FAILED|crashed with|assertion|SIGSEGV|AddressSanitizer" "no crash or assertion in the log"
# The fuzzer may be disconnected by a protocol error (that is the protocol's
# isolation working); a timeout or a `timeout` kill is not acceptable.
sde_test_expect "the fuzzer completed or was cleanly disconnected" test "$status" -eq 0

# And it can still serve a fresh client, not just stay alive.
before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 5 "$NATIVE" --title "Wayland Fuzz After" > "$RUNTIME/after.log" 2>&1 || true
mapped_more() { [[ "$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)" -gt "${before:-0}" ]]; }
for _ in $(seq 1 50); do
    mapped_more && break
    sleep 0.1
done
sde_test_expect "a client still maps after the fuzz stream" mapped_more

sde_test_finish
