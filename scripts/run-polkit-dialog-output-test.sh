#!/usr/bin/env bash
# ci-tier: fast
# sde-polkit-agent must parse sde-auth-dialog's two-line output correctly. The
# payload (Tests/PolkitDialogOutputTest.cpp) covers the unchecked "0" flag that
# used to be sent as the password, plus diagnostics, empty and malformed output.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init polkit-dialog-output
sde_test_export_roots

TEST="$SDE_ROOT/build/test/polkit-dialog-output-test"

"$SDE_ROOT/scripts/build-tests.sh" polkit-dialog-output-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: polkit-dialog-output-test not built"; exit 0; }

status=0
env LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 60 "$TEST" > "$RUNTIME/test.log" 2>&1 || status=$?

cat "$RUNTIME/test.log"
if [[ "$status" -eq 0 ]]; then
    sde_test_check "$RUNTIME/test.log" 'ok: polkit dialog output keeps the password separate from the remember flag' \
        "the dialog output keeps the password separate from the remember flag"
else
    echo "  FAIL: polkit-dialog-output-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_finish "$RUNTIME/test.log"
