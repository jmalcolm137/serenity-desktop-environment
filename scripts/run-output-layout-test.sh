#!/usr/bin/env bash
# Screen-layout change test (R6/N5).
#
# A layout change has to reach the live outputs, not just the file. That chain is
# what Display Settings' Monitor tab uses: the widget calls the shim's
# set_screen_layout, the compositor persists it (Config::set_outputs), notices the
# file change (Config::watch_outputs) and applies mode/scale/position to each
# output (apply_loaded_screen_layout). Until the compositor did the last part, a
# layout change was invisible until the next session — which is why the port had
# disabled the tab with the note that "the host compositor does not implement
# output configuration".
#
# This exercises the same path from the file side, which is the side a client's
# change ends up on: change the layout, then ask the compositor what it has the
# output at now. Runs headless and hermetic.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init output-layout
sde_test_export_roots
OM="$SDE_ROOT/build/tools/output-management-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$OM" ]] || { echo "SKIP: output-management-test not built"; exit 0; }

mkdir -p "$RUNTIME/state"
# Deliberately *not* SDE_OUTPUT_SCALE (the empty value clears the library's): that
# override wins over the persisted layout by design, and this test is about the
# layout being applied.
sde_test_compositor SDE_OUTPUT_SCALE= SDE_STATE_DIR="$RUNTIME/state" \
    SDE_SESSION_DIR="$RUNTIME/session" SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" SDE_XWAYLAND=0
sde_test_wait_log "$RUNTIME/comp.log" "applied screen layout" 200 || true
sleep 0.5

query() { env WAYLAND_DISPLAY="$SOCK" timeout 10 "$OM" --query "$1" 2>/dev/null | head -1; }

OUT="$(grep -o 'HEADLESS-[0-9]*' "$RUNTIME/comp.log" | head -1)"
echo "=== the output starts at the backend's mode ==="
BEFORE="$(query "$OUT")"
echo "  $BEFORE"
sde_test_expect "1280x720 at 1x" test "$BEFORE" = "head $OUT mode=1280x720 scale=1"

echo "=== a layout change (what the Monitor tab's Apply writes) ==="
cat > "$RUNTIME/state/screenlayout.ini" <<INI
[Screens]
MainScreen=0

[Screen0]
Device=$OUT
Width=800
Height=600
Top=0
Left=0
Mode=Device
ScaleFactor=2
INI
sleep 1.5
AFTER="$(query "$OUT")"
echo "  $AFTER"
sde_test_expect "the live output is now 800x600 at 2x" test "$AFTER" = "head $OUT mode=800x600 scale=2"
sde_test_expect "the compositor reported the new layout once" \
    test "$(grep -c "applied screen layout to '$OUT': 800x600 @2x" "$RUNTIME/comp.log")" = 1
sde_test_expect "the compositor survived" kill -0 "$COMP_PID"

sde_test_finish
