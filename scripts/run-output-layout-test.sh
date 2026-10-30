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
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
OM="$SDE_ROOT/build/tools/output-management-test"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$OM" ]] || { echo "SKIP: output-management-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-layout.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-layout-$$"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"{,.lock}
}
trap cleanup EXIT

mkdir -p "$RUNTIME/state"
# Deliberately *not* SDE_OUTPUT_SCALE: that override wins over the persisted layout
# by design, and this test is about the layout being applied.
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    -u SDE_OUTPUT_SCALE \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME/state" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SDE_XWAYLAND=0 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 200); do
    [[ "$(grep -c 'applied screen layout' "$RUNTIME/comp.log" 2>/dev/null)" -ge 1 ]] && break
    sleep 0.1
done
sleep 0.5

fail=0
check() {
    if [[ "$2" == "$3" ]]; then echo "  ok: $1"; else echo "  MISSING: $1 (expected '$2', got '$3')"; fail=1; fi
}
query() { env WAYLAND_DISPLAY="$SOCK" timeout 10 "$OM" --query "$1" 2>/dev/null | head -1; }

OUT="$(grep -o 'HEADLESS-[0-9]*' "$RUNTIME/comp.log" | head -1)"
echo "=== the output starts at the backend's mode ==="
BEFORE="$(query "$OUT")"
echo "  $BEFORE"
check "1280x720 at 1x" "head $OUT mode=1280x720 scale=1" "$BEFORE"

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
check "the live output is now 800x600 at 2x" "head $OUT mode=800x600 scale=2" "$AFTER"
check "the compositor reported the new layout" "1" \
    "$(grep -c "applied screen layout to '$OUT': 800x600 @2x" "$RUNTIME/comp.log")"
if kill -0 "$COMP_PID" 2>/dev/null; then echo "  ok: the compositor survived"; else echo "  MISSING: the compositor died"; fail=1; fi

if [[ "$fail" -eq 0 ]]; then
    echo "==> screen layout test PASSED"
else
    echo "==> screen layout test FAILED"
    echo "--- compositor log (tail) ---"; tail -20 "$RUNTIME/comp.log" | sed 's/^/  /'
    exit 1
fi
