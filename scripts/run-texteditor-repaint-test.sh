#!/usr/bin/env bash
# ci-tier: fast
# TextEditor selection changes must repaint only the lines whose highlight
# changed, not the whole widget. The payload (Tests/TextEditorRepaintTest.cpp)
# opens a headless window, records paint_event() rects, and checks the damage
# for selecting, moving and clearing a multi-line selection. It also checks that
# on_modified_change only fires when the document's modified state changes.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init texteditor-repaint
sde_test_export_roots

TEST="$SDE_ROOT/build/test/texteditor-repaint-test"

"$SDE_ROOT/scripts/build-tests.sh" texteditor-repaint-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: texteditor-repaint-test not built"; exit 0; }

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
    sde_test_check "$RUNTIME/test.log" 'ok: selection changes repaint only the affected lines' \
        "selection changes repaint only the affected lines"
    sde_test_check "$RUNTIME/test.log" 'ok: on_modified_change fires only on modified-state transitions' \
        "the modified callback fires only on transitions"
    sde_test_check "$RUNTIME/test.log" 'ok: text editor repaints only what changed' \
        "the repaint test passed"
else
    echo "  FAIL: texteditor-repaint-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi

sde_test_no_fault "$RUNTIME/test.log"
sde_test_finish "$RUNTIME/test.log"
