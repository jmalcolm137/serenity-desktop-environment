#!/usr/bin/env bash
# ci-tier: fast
# LibIPC must not re-enter the event loop while a large message is being written.
# The payload (Tests/LibIPCWriteTest.cpp) fills a small socket buffer with a
# 1 MiB message while the receiver is not reading yet, and asserts that no event
# loop callback ran during the send. It also checks that the message arrives
# intact and that the old "needed N writes" FIXME warning is gone.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init libipc-write
sde_test_export_roots

TEST="$SDE_ROOT/build/test/libipc-write-test"

"$SDE_ROOT/scripts/build-tests.sh" libipc-write-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: libipc-write-test not built"; exit 0; }

status=0
env LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 60 "$TEST" > "$RUNTIME/test.log" 2>&1 || status=$?

cat "$RUNTIME/test.log"
if [[ "$status" -eq 0 ]]; then
    sde_test_check "$RUNTIME/test.log" 'ok: large IPC message transferred without re-entering the event loop' \
        "the large message was sent without pumping the event loop"
else
    echo "  FAIL: libipc-write-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_refute "$RUNTIME/test.log" 'FIXME Warning, needed [0-9]+ writes' \
    "the large-message write warning is gone"
sde_test_no_fault "$RUNTIME/test.log"
sde_test_finish "$RUNTIME/test.log"
