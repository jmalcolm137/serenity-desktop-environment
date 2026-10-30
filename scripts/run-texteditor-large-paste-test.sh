#!/usr/bin/env bash
# ci-tier: fast
# Pasting a 1 MiB document into TextEditor must be one batched edit, not one
# notification per character. The payload measures the insertion and checks the
# content; the old quadratic path exceeds the two-second limit.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init texteditor-large-paste
sde_test_export_roots

TEST="$SDE_ROOT/build/test/texteditor-large-paste-test"

"$SDE_ROOT/scripts/build-tests.sh" texteditor-large-paste-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: texteditor-large-paste-test not built"; exit 0; }

SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

sde_test_compositor

status=0
env WAYLAND_DISPLAY="$SOCK" \
    SDE_SESSION_DIR="$SESSION_DIR" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 60 "$TEST" > "$RUNTIME/test.log" 2>&1 || status=$?

cat "$RUNTIME/test.log"
if [[ "$status" -eq 0 ]]; then
    sde_test_check "$RUNTIME/test.log" 'ok: 1 MiB text inserted in [0-9]+ ms' \
        "a 1 MiB paste is inserted in one batched edit"
else
    echo "  FAIL: texteditor-large-paste-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_no_fault "$RUNTIME/test.log"
sde_test_finish "$RUNTIME/test.log"
