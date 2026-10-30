#!/usr/bin/env bash
# ci-tier: fast
# A transient child must survive the destruction of its parent. The payload opens
# a parent and a blocking "Unsaved Changes" dialog, closes the parent, and leaves
# the dialog alive. The driver moves the pointer over the dialog; the compositor
# must not crash walking the modal chain.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init transient-parent
sde_test_export_roots

TEST="$SDE_ROOT/build/test/transient-parent-test"

sde_test_build WindowServer
"$SDE_ROOT/scripts/build-tests.sh" transient-parent-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: transient-parent-test not built"; exit 0; }

VI="$(sde_test_vi)"
sde_test_compositor

SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

env WAYLAND_DISPLAY="$SOCK" \
    SDE_SESSION_DIR="$SESSION_DIR" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$TEST" > "$RUNTIME/test.log" 2>&1 &
TEST_PID=$!

if ! sde_test_wait_log "$RUNTIME/test.log" '^READY$' 100; then
    echo "  FAIL: the transient-parent test did not become ready"
    kill "$TEST_PID" 2>/dev/null || true
    wait "$TEST_PID" 2>/dev/null || true
    cat "$RUNTIME/test.log"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
    sde_test_finish "$RUNTIME/test.log"
fi

# The dialog is centred by the compositor; move the pointer over it. Any motion
# hit-tests the surviving child and walks its modal chain.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
    --move 640 360 --sleep 200 --move 650 370 --linger 300 >/dev/null 2>&1 || true

status=0
for _ in $(seq 1 100); do
    if ! kill -0 "$TEST_PID" 2>/dev/null; then
        wait "$TEST_PID" || status=$?
        break
    fi
    sleep 0.1
done
if kill -0 "$TEST_PID" 2>/dev/null; then
    echo "  FAIL: the transient-parent test did not finish"
    kill "$TEST_PID" 2>/dev/null || true
    wait "$TEST_PID" 2>/dev/null || true
    status=124
fi

cat "$RUNTIME/test.log"
if grep -q '^SURVIVED$' "$RUNTIME/test.log" && kill -0 "$COMP_PID" 2>/dev/null; then
    echo "  ok: the compositor survived a transient child outliving its parent"
else
    echo "  FAIL: the compositor or client did not survive"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
if grep -qiE 'SIGSEGV|VERIFICATION FAILED|ASSERTION FAILED' "$RUNTIME/comp.log" "$RUNTIME/test.log"; then
    echo "  FAIL: a process faulted"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
else
    echo "  ok: no crash or assertion"
fi
sde_test_finish "$RUNTIME/test.log"