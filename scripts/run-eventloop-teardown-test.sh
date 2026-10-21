#!/usr/bin/env bash
# A deferred callback with no event loop must not crash (the teardown path).
#
# A GUI application exiting because its compositor went away has destroyed its
# EventLoop while widget trees are still being destroyed, and destroying a widget can
# defer work. `EventLoop::current()` returned a reference into the empty loop stack
# there, so clients dumped core around a compositor restart. The payload is the test
# (Tests/EventLoopTeardownTest.cpp) and its exit status is the assertion: dying by
# signal is the bug.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

"$SDE_ROOT/scripts/build-tests.sh" >/dev/null
TEST="$SDE_ROOT/build/test/libwm-eventloop-teardown-test"
[[ -x "$TEST" ]] || { echo "SKIP: libwm-eventloop-teardown-test not built"; exit 0; }

status=0
OUTPUT="$(LD_LIBRARY_PATH="$BUILD/lib" "$TEST" 2>&1)" || status=$?
if (( status >= 128 )); then
    echo "  MISSING: died from signal $((status - 128)) (SIGSEGV is 11, i.e. status 139)"
    echo "$OUTPUT" | tail -8 | sed 's/^/    /'
    echo "==> eventloop teardown test FAILED"
    exit 1
fi
if (( status != 0 )); then
    echo "  MISSING: exited $status, and the deferred callback ran during teardown:"
    echo "$OUTPUT" | tail -4 | sed 's/^/    /'
    echo "==> eventloop teardown test FAILED"
    exit 1
fi
echo "  ok: $(echo "$OUTPUT" | tail -1)"
echo "==> eventloop teardown test PASSED"
