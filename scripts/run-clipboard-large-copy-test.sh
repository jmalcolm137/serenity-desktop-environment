#!/usr/bin/env bash
# ci-tier: fast
# A large clipboard copy must not kill the application through the clipboard IPC
# connection. The payload (Tests/ClipboardLargeCopyTest.cpp) opens a window,
# waits for a click (the host only accepts a selection with an input serial),
# copies 1 MiB, and synchronously fetches it from a clipboard client callback.
# It only passes if the process survives with the data intact.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init clipboard-large-copy
sde_test_export_roots

TEST="$SDE_ROOT/build/test/clipboard-large-copy-test"

"$SDE_ROOT/scripts/build-tests.sh" clipboard-large-copy-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: clipboard-large-copy-test not built"; exit 0; }

SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

sde_test_compositor
VI="$(sde_test_vi)"

env WAYLAND_DISPLAY="$SOCK" \
    SDE_SESSION_DIR="$SESSION_DIR" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$TEST" > "$RUNTIME/test.log" 2>&1 &
TEST_PID=$!

if ! sde_test_wait_log "$RUNTIME/test.log" '^READY$' 100; then
    echo "  FAIL: the clipboard test window did not become ready"
    kill "$TEST_PID" 2>/dev/null || true
    wait "$TEST_PID" 2>/dev/null || true
    cat "$RUNTIME/test.log"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
    sde_test_finish "$RUNTIME/test.log"
fi

env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
    --move 150 150 \
    --button 0x110 press --sleep 100 --button 0x110 release --sleep 500 \
    --button 0x110 press --sleep 100 --button 0x110 release --linger 300 \
    >/dev/null 2>&1 || true

status=0
for _ in $(seq 1 200); do
    if ! kill -0 "$TEST_PID" 2>/dev/null; then
        wait "$TEST_PID" || status=$?
        break
    fi
    sleep 0.1
done
if kill -0 "$TEST_PID" 2>/dev/null; then
    echo "  FAIL: the clipboard test did not finish"
    kill "$TEST_PID" 2>/dev/null || true
    wait "$TEST_PID" 2>/dev/null || true
    status=124
fi

cat "$RUNTIME/test.log"
if [[ "$status" -eq 0 ]]; then
    sde_test_check "$RUNTIME/test.log" 'ok: a large clipboard copy keeps the connection alive and the data intact' \
        "a large clipboard copy keeps the connection alive"
else
    echo "  FAIL: clipboard-large-copy-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_no_fault "$RUNTIME/test.log"
sde_test_finish "$RUNTIME/test.log"
