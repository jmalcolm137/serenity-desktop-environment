#!/usr/bin/env bash
# ci-tier: fast
# TextEditor mouse hit-testing must agree with the visual layout that
# recompute_all_visual_lines() produced, without scanning every line.
#
# text_position_at_content_position() used to walk all lines twice and call
# Document::line_is_visible() (itself a walk over the folding regions) for each
# one. It runs on every mouse move, on selection drags and four times per
# paint_event, so a large file made the editor crawl. It now binary-searches the
# visual-rect tops. The payload (Tests/TextEditorHitTest.cpp) checks the new
# lookup against the old definition for every y: uniform lines, folded regions
# (zero-height rects), wrapped multi-row lines, negative y and past the end.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init texteditor-hittest
sde_test_export_roots

TEST="$SDE_ROOT/build/test/texteditor-hittest-test"

"$SDE_ROOT/scripts/build-tests.sh" texteditor-hittest-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: texteditor-hittest-test not built"; exit 0; }

# LibGUI::Application needs the session's clipboard endpoint, which LibWM exposes
# in the compositor; give it a session directory like the other LibGUI tests.
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
    sde_test_check "$RUNTIME/test.log" 'ok: text editor hit-testing matches the visual line layout' \
        "the hit-test matches the visual line layout"
else
    echo "  FAIL: texteditor-hittest-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi

sde_test_finish "$RUNTIME/test.log"
