#!/usr/bin/env bash
# Headless session-lock test (R4.1, docs/11-r4-implementation-plan.md). The
# compositor is a standard ext-session-lock-v1 server, so a conformant locker
# (swaylock and friends) works against it. Two phases:
#
#   (A) a protocol-level lock client covers every output and gets 'locked';
#   (B) Super+L spawns the configured lock command (SDE_LOCK_CMD), which locks.
#
# Runs entirely headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/lock-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
LOCK_XML="$WP/staging/ext-session-lock/ext-session-lock-v1.xml"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
wayland-scanner client-header "$LOCK_XML" "$TOOLS_BUILD/ext-session-lock-v1-client-protocol.h"
wayland-scanner private-code  "$LOCK_XML" "$TOOLS_BUILD/ext-session-lock-v1-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/lock-test.c" \
    "$TOOLS_BUILD/ext-session-lock-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/lock-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"
OM_XML="$WLROOTS_SRC/protocol/wlr-output-management-unstable-v1.xml"
wayland-scanner client-header "$OM_XML" "$TOOLS_BUILD/wlr-output-management-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$OM_XML" "$TOOLS_BUILD/wlr-output-management-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/output-mode-test.c" \
    "$TOOLS_BUILD/wlr-output-management-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/output-mode-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-lock.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

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

start_compositor() {
    local sock="$1" outputs="$2"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless \
        WLR_HEADLESS_OUTPUTS="$outputs" \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm-$sock" \
        SDE_LOCK_CMD="${SDE_LOCK_CMD:-}" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$RUNTIME/comp-$sock.log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 50); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    sleep 1
}
stop_compositor() {
    kill "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
}

echo "== (A) a standard lock client covers every output =="
start_compositor lock-a 2
env WAYLAND_DISPLAY=lock-a timeout 15 "$TOOLS_BUILD/lock-test" --hold 600 > "$RUNTIME/lock-a.log" 2>&1 || true
sleep 1
stop_compositor
cat "$RUNTIME/lock-a.log"

check "LOCK-TEST: lock surface created" "$RUNTIME/lock-a.log"
check "LOCK-TEST: locked" "$RUNTIME/lock-a.log"
check "LOCK-TEST: unlocking" "$RUNTIME/lock-a.log"
check "WindowServer: session locked" "$RUNTIME/comp-lock-a.log"
check "WindowServer: session unlocked" "$RUNTIME/comp-lock-a.log"
surface_count="$(grep -cF "WindowServer: session lock surface for output" "$RUNTIME/comp-lock-a.log" || true)"
covered_count="$(grep -cF "WindowServer: session lock surface covered an output" "$RUNTIME/comp-lock-a.log" || true)"
if [[ "$surface_count" -ge 2 && "$covered_count" -ge 2 ]]; then
    echo "  ok: every output covered before 'locked' (surfaces=$surface_count covered=$covered_count)"
else
    echo "  MISSING: every output covered (surfaces=$surface_count covered=$covered_count)"
    fail=1
fi

echo "== (B) Super+L spawns the configured lock command =="
SDE_LOCK_CMD="$TOOLS_BUILD/lock-test --hold 500" start_compositor lock-b 1
# Super_L (evdev 125) + L (evdev 38).
env WAYLAND_DISPLAY=lock-b "$TOOLS_BUILD/virtual-input-test" \
    --key 125 press --key 38 press --key 38 release --key 125 release --sleep 200 \
    > "$RUNTIME/key.log" 2>&1 || true
sleep 2
stop_compositor
cat "$RUNTIME/comp-lock-b.log"

check "WindowServer: locking via" "$RUNTIME/comp-lock-b.log"
check "WindowServer: session locked" "$RUNTIME/comp-lock-b.log"

echo "== (C) lock surface reconfigures when an output changes size =="
start_compositor lock-c 1
env WAYLAND_DISPLAY=lock-c "$TOOLS_BUILD/lock-test" --hold 3000 > "$RUNTIME/lock-c.log" 2>&1 &
LOCK_C_PID=$!
# Wait for the lock to be established (surfaces configured at the initial size)
# before resizing, so the resize exercises the re-configure path rather than
# racing the lock surface's initial configure.
for _ in $(seq 1 50); do
    grep -qF "LOCK-TEST: locked" "$RUNTIME/lock-c.log" && break
    sleep 0.1
done
env WAYLAND_DISPLAY=lock-c "$TOOLS_BUILD/output-mode-test" 1024x768 > "$RUNTIME/mode.log" 2>&1 || true
# Wait for the lock client to ack the re-configure (it prints when it processes
# the new configure) before stopping it.
for _ in $(seq 1 50); do
    grep -qF "LOCK-TEST: surface configured 1024x768" "$RUNTIME/lock-c.log" && break
    sleep 0.1
done
kill "$LOCK_C_PID" 2>/dev/null || true
wait "$LOCK_C_PID" 2>/dev/null || true
stop_compositor
cat "$RUNTIME/mode.log"
grep "surface configured" "$RUNTIME/lock-c.log" || true

check "WindowServer: lock surface re-configured" "$RUNTIME/comp-lock-c.log"
check "LOCK-TEST: surface configured 1024x768" "$RUNTIME/lock-c.log"

if [[ "$fail" -ne 0 ]]; then
    echo "==> session-lock test FAILED"
    exit 1
fi
echo "==> session-lock test PASSED"
