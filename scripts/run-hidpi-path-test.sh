#!/usr/bin/env bash
# Regression test: a filled Gfx::Path must render at >1x (HiDPI), not crash.
#
# EdgeFlagPathRasterizer used to VERIFY(painter.scale() == 1), which aborted
# SystemMonitor's Performance tab (GraphWidget fills the area under the graph) on
# a 2x output. We render that tab at 2x and require a non-empty backing store
# with no verification failure (port patch 0027).
#
# Screenshot/backing-store based (D7). Runs headless, so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init hidpi-path
sde_test_export_roots
APP="$BUILD/bin/SystemMonitor"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
if [[ ! -x "$APP" ]]; then
    echo "==> SystemMonitor not built at $APP"
    exit 1
fi

sde_test_compositor --startup "$APP -t graphs" \
    SDE_OUTPUT_SCALE=2 SDE_WM_PORTAL_PATH="$RUNTIME/wm" LIBWM_DUMP="$RUNTIME/2x.png"

# The app dumps its 2x backing store once it has painted.
for _ in $(seq 1 80); do
    [[ -s "$RUNTIME/2x.png" ]] && break
    sleep 0.1
done
sleep 1
sde_test_stop_compositor

echo "=== HiDPI filled-path test (2x) ==="
if [[ -s "$RUNTIME/2x.png" ]]; then
    echo "  ok: 2x backing store rendered ($(stat -c%s "$RUNTIME/2x.png") bytes)"
else
    echo "  FAIL: no 2x backing store dump"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi
sde_test_refute "$RUNTIME/comp.log" "VERIFICATION FAILED" "no verification failure while filling a path at 2x"
sde_test_finish
