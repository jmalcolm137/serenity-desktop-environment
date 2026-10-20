#!/usr/bin/env bash
# ci-tier: full
# Headless session-safety-net test (R4.1, docs/11-r4-implementation-plan.md):
# assert the compositor implements ext-idle-notify-v1, zwp_idle_inhibit_v1 and
# wlr-output-power-management, and that idle inhibition suppresses the idle
# notification. Runs entirely headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init idle
sde_test_export_roots
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/idle-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
IDLE_XML="$WP/staging/ext-idle-notify/ext-idle-notify-v1.xml"
INHIBIT_XML="$WP/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml"
POWER_XML="$WLROOTS_SRC/protocol/wlr-output-power-management-unstable-v1.xml"

wayland-scanner client-header "$IDLE_XML" "$TOOLS_BUILD/ext-idle-notify-v1-client-protocol.h"
wayland-scanner private-code  "$IDLE_XML" "$TOOLS_BUILD/ext-idle-notify-v1-protocol.c"
wayland-scanner client-header "$INHIBIT_XML" "$TOOLS_BUILD/idle-inhibit-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$INHIBIT_XML" "$TOOLS_BUILD/idle-inhibit-unstable-v1-protocol.c"
wayland-scanner client-header "$POWER_XML" "$TOOLS_BUILD/wlr-output-power-management-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$POWER_XML" "$TOOLS_BUILD/wlr-output-power-management-unstable-v1-protocol.c"

cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/idle-test.c" \
    "$TOOLS_BUILD/ext-idle-notify-v1-protocol.c" \
    "$TOOLS_BUILD/idle-inhibit-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-output-power-management-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/idle-test"
# The portable idle daemon (sde-idle) and sde-dpms, built by the shared tools script.
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
SDE_IDLE="$SDE_ROOT/build/tools/sde-idle"
SDE_DPMS="$SDE_ROOT/build/tools/sde-dpms"

# The virtual pointer, to trigger a "resumed" (activity) event.
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"

SOCK="sde-idle-$$"

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"

run_client() { env WAYLAND_DISPLAY="$SOCK" timeout 15 "$TOOLS_BUILD/idle-test" "$@"; }

echo "== (A) idle notification fires with no input =="
run_client --timeout 500 --hold 1500 > "$RUNTIME/idle.log" 2>&1 || true
cat "$RUNTIME/idle.log"

echo "== (B) an idle inhibitor suppresses it =="
run_client --timeout 500 --hold 1500 --inhibit > "$RUNTIME/inhibit.log" 2>&1 || true
cat "$RUNTIME/inhibit.log"

echo "== (C) DPMS off/on =="
run_client --dpms off --hold 400 > "$RUNTIME/dpms-off.log" 2>&1 || true
run_client --dpms on --hold 400 > "$RUNTIME/dpms-on.log" 2>&1 || true
cat "$RUNTIME/dpms-off.log" "$RUNTIME/dpms-on.log"

echo "== (D) sde-idle runs its command on idle =="
env WAYLAND_DISPLAY="$SOCK" "$SDE_IDLE" timeout 1 "touch $RUNTIME/idle-fired" \
    > "$RUNTIME/sde-idle.log" 2>&1 &
SDE_IDLE_PID=$!
sleep 2.5
kill "$SDE_IDLE_PID" 2>/dev/null || true
wait "$SDE_IDLE_PID" 2>/dev/null || true
cat "$RUNTIME/sde-idle.log"

echo "== (E) sde-idle resume + sde-dpms (blank before lock) =="
# sde-dpms blanks/restores the output via wlr-output-power-management.
env WAYLAND_DISPLAY="$SOCK" "$SDE_DPMS" off > "$RUNTIME/sde-dpms-off.log" 2>&1 || true
env WAYLAND_DISPLAY="$SOCK" "$SDE_DPMS" on > "$RUNTIME/sde-dpms-on.log" 2>&1 || true
# sde-idle: lock after 1s, then a pointer move triggers the resume command.
env WAYLAND_DISPLAY="$SOCK" "$SDE_IDLE" \
    timeout 1 "touch $RUNTIME/idle2" resume "touch $RUNTIME/resumed2" \
    > "$RUNTIME/sde-idle-resume.log" 2>&1 &
RESUME_PID=$!
sleep 2
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" \
    --extent 1280 720 --move 400 300 --sleep 200 > /dev/null 2>&1 || true
sleep 1.5
kill "$RESUME_PID" 2>/dev/null || true
wait "$RESUME_PID" 2>/dev/null || true
cat "$RUNTIME/sde-idle-resume.log"

sleep 0.5
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
    return 0
}
check_absent() {
    if grep -qF "$1" "$2"; then
        echo "  UNEXPECTED ($2): $1"
        fail=1
    else
        echo "  ok (absent): $1"
    fi
    return 0
}

echo "=== session safety net test ==="
check "IDLE-TEST: notification timeout=500" "$RUNTIME/idle.log"
check "IDLE-TEST: idled" "$RUNTIME/idle.log"

check "IDLE-TEST: inhibited" "$RUNTIME/inhibit.log"
check_absent "IDLE-TEST: idled" "$RUNTIME/inhibit.log"
check "WindowServer: idle inhibitor created" "$RUNTIME/comp.log"

check "IDLE-TEST: dpms off" "$RUNTIME/dpms-off.log"
check "IDLE-TEST: dpms on" "$RUNTIME/dpms-on.log"
check "output 'HEADLESS-1' off" "$RUNTIME/comp.log"
check "output 'HEADLESS-1' on" "$RUNTIME/comp.log"

# sde-idle (the portable idle-to-action daemon) ran its command.
check "sde-idle: armed timeout 1s" "$RUNTIME/sde-idle.log"
if [[ -e "$RUNTIME/idle-fired" ]]; then
    echo "  ok: sde-idle ran its command on idle"
else
    echo "  MISSING: sde-idle did not run its command"
    fail=1
fi

# sde-dpms blanked/restored the output; sde-idle ran its resume command on activity.
check "sde-idle: resumed; running" "$RUNTIME/sde-idle-resume.log"
for f in idle2 resumed2; do
    if [[ -e "$RUNTIME/$f" ]]; then
        echo "  ok: sde-idle ran '$f'"
    else
        echo "  MISSING: sde-idle did not run '$f'"
        fail=1
    fi
done

if [[ "$fail" -ne 0 ]]; then
    echo "==> session safety net test FAILED"
    exit 1
fi
echo "==> session safety net test PASSED"
