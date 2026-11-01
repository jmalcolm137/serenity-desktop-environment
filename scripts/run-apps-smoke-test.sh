#!/usr/bin/env bash
# ci-tier: full
# Smoke test: launch a set of protocol-heavy Serenity apps against the compositor
# and assert none of them fault or print an IPC parse error.
#
# This catches a whole class of regressions: the WindowServer IPC `MessageID` is
# positional, so inserting a method mid-interface shifts every later id and breaks
# apps built against the old enum ("Failed to parse IPC message") until everything
# is rebuilt. New methods must be appended (see libgui-wayland patch 0042).
#
# Runs headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init apps-smoke
sde_test_export_roots

# Settings apps exercise the WindowServer protocol hardest (mouse/keyboard/display
# get/set), so they surfaced the positional-id bug.
APPS=(MouseSettings KeyboardSettings DisplaySettings SystemMonitor FileManager TextEditor ThemeEditor Calculator)

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"

FAULTS='Failed to parse IPC message|SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error'
echo "=== each app maps a window without faulting ==="
before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
for app in "${APPS[@]}"; do
    if [[ ! -x "$BUILD/bin/$app" ]]; then
        echo "  SKIP: $app (not built)"
        continue
    fi
    log="$RUNTIME/$app.log"
    env WAYLAND_DISPLAY="$SOCK" SDE_APP_BIN_DIR="$BUILD/bin" \
        timeout 5 "$BUILD/bin/$app" > "$log" 2>&1 || true
    after="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
    if grep -qiE "$FAULTS" "$log"; then
        echo "  FAIL: $app: $(grep -iE "$FAULTS" "$log" | head -1)"
        echo "      --- $app log ---"; tail -6 "$log" 2>/dev/null | sed 's/^/      /' || true
        SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
    else
        sde_test_expect "$app" test "$after" -gt "$before"
    fi
    before="$after"
done

sde_test_stop_compositor
sde_test_finish
