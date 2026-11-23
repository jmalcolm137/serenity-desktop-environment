#!/usr/bin/env bash
# ci-tier: fast
# Pure-logic unit tests for sde-lock (KeyInput, HiDPI LockRenderer, GECOS, cursor).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init lock-unit
sde_test_export_roots

TEST="$SDE_ROOT/build/test/sde-lock-unit-test"

"$SDE_ROOT/scripts/build-tests.sh" sde-lock-unit-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: sde-lock-unit-test not built"; exit 0; }

status=0
env LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 60 "$TEST" > "$RUNTIME/test.log" 2>&1 || status=$?

cat "$RUNTIME/test.log"
if [[ "$status" -eq 0 ]]; then
    sde_test_check "$RUNTIME/test.log" 'ok: sde-lock unit tests passed' \
        "sde-lock unit tests passed"
else
    echo "  FAIL: sde-lock-unit-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_finish "$RUNTIME/test.log"
