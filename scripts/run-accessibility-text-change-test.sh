#!/usr/bin/env bash
# ci-tier: fast
# Text-change accessibility notifications must not rebuild and send the whole
# editor document. The payload (Tests/AccessibilityTextChangeTest.cpp) creates a
# 4 MiB editor, sends 50 notifications on a private AT-SPI bus, and requires them
# to finish in under a second.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init accessibility-text-change
sde_test_export_roots

TEST="$SDE_ROOT/build/test/accessibility-text-change-test"
DBUS_PID=""

"$SDE_ROOT/scripts/build-tests.sh" accessibility-text-change-test >/dev/null 2>&1 || true
[[ -x "$TEST" ]] || { echo "SKIP: accessibility-text-change-test not built"; exit 0; }
command -v dbus-daemon >/dev/null || { echo "SKIP: dbus-daemon is not installed"; exit 0; }

sde_test_cleanup_extra() {
    [[ -n "$DBUS_PID" ]] && kill "$DBUS_PID" 2>/dev/null || true
    [[ -n "$DBUS_PID" ]] && wait "$DBUS_PID" 2>/dev/null || true
    DBUS_PID=""
}

DBUS_INFO="$(dbus-daemon --session --fork --print-address=1 --print-pid=1)"
DBUS_ADDRESS="$(sed -n '1p' <<<"$DBUS_INFO")"
DBUS_PID="$(sed -n '2p' <<<"$DBUS_INFO")"

SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

sde_test_compositor

status=0
env WAYLAND_DISPLAY="$SOCK" \
    SDE_SESSION_DIR="$SESSION_DIR" \
    AT_SPI_BUS_ADDRESS="$DBUS_ADDRESS" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 60 "$TEST" > "$RUNTIME/test.log" 2>&1 || status=$?

cat "$RUNTIME/test.log"
if [[ "$status" -eq 0 ]]; then
    sde_test_check "$RUNTIME/test.log" 'ok: accessibility text notifications do not copy the document' \
        "text-change notifications do not copy the whole document"
else
    echo "  FAIL: accessibility-text-change-test exited with status $status"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_no_fault "$RUNTIME/test.log"
sde_test_finish "$RUNTIME/test.log"
