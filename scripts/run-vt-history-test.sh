#!/usr/bin/env bash
# ci-tier: fast
# "Clear to previous mark" must not crash the Terminal once the scrollback has
# wrapped.
#
# The VT history is a ring buffer indexed by m_history_start, but
# Terminal::clear_to_mark() (reached from TerminalWidget::clear_to_previous_mark)
# removed lines from it without maintaining that index. A mark whose line the ring
# had since overwritten stayed in the mark set, so clearing to it wiped the whole
# history and left the index pointing past the end. The next linefeed then scrolled
# a line into history and inserted at m_history_start - 1, out of bounds --
# AK::Vector::insert's `index <= size()` VERIFY, i.e. SIGILL. The payload is the
# test (Tests/VtHistoryTest.cpp) and its exit status is the assertion: dying by
# signal is the bug.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

"$SDE_ROOT/scripts/build-tests.sh" libvt-history-test >/dev/null
TEST="$SDE_ROOT/build/test/libvt-history-test"
[[ -x "$TEST" ]] || { echo "SKIP: libvt-history-test not built"; exit 0; }

status=0
OUTPUT="$(LD_LIBRARY_PATH="$BUILD/lib" "$TEST" 2>&1)" || status=$?
if (( status >= 128 )); then
    echo "  MISSING: died from signal $((status - 128)) (SIGILL is 4, i.e. status 132)"
    echo "$OUTPUT" | tail -8 | sed 's/^/    /'
    echo "==> vt history test FAILED"
    exit 1
fi
if (( status != 0 )); then
    echo "  MISSING: exited $status:"
    echo "$OUTPUT" | tail -4 | sed 's/^/    /'
    echo "==> vt history test FAILED"
    exit 1
fi
echo "  ok: $(echo "$OUTPUT" | tail -1)"
echo "==> vt history test PASSED"
