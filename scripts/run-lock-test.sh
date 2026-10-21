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
# The native locker; the phase below skips if it cannot be built.
"$SDE_ROOT/scripts/build-target.sh" sde-lock >/dev/null 2>&1 || true

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
    "$SDE_ROOT/Tools/lock-test.c" \
    "$TOOLS_BUILD/ext-session-lock-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/lock-test"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"
OM_XML="$WLROOTS_SRC/protocol/wlr-output-management-unstable-v1.xml"
wayland-scanner client-header "$OM_XML" "$TOOLS_BUILD/wlr-output-management-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$OM_XML" "$TOOLS_BUILD/wlr-output-management-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/output-mode-test.c" \
    "$TOOLS_BUILD/wlr-output-management-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/output-mode-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-lock.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

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

echo "== (D) the native SDE locker =="
if [[ -x "$SDE_ROOT/build/tools/sde-lock" ]]; then
    start_compositor lock-d 1
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        env WAYLAND_DISPLAY=lock-d timeout 30 "$SDE_ROOT/build/tools/sde-lock" > "$RUNTIME/lock-d.log" 2>&1 &
    LOCK_D_PID=$!
    for _ in $(seq 1 80); do
        grep -qF "WindowServer: session locked" "$RUNTIME/comp-lock-d.log" && break
        sleep 0.1
    done
    check "WindowServer: session locked" "$RUNTIME/comp-lock-d.log"
    d_covered="$(grep -cF "WindowServer: session lock surface covered an output" "$RUNTIME/comp-lock-d.log" || true)"
    if [[ "$d_covered" -ge 1 ]]; then
        echo "  ok: the native locker covered the output (surfaces=$d_covered)"
    else
        echo "  MISSING: the native locker covered no output"
        fail=1
    fi

    # PAM: type a wrong password and Enter. The client must report the failure and
    # stay locked ("login" is the default service; the password is deliberately
    # wrong, so no account is touched). evdev: w=17 r=19 o=24 n=49 g=34 Enter=28.
    env WAYLAND_DISPLAY=lock-d "$TOOLS_BUILD/virtual-input-test" \
        --key 17 press --key 17 release --key 19 press --key 19 release \
        --key 24 press --key 24 release --key 49 press --key 49 release \
        --key 34 press --key 34 release --key 28 press --key 28 release \
        --sleep 200 > "$RUNTIME/type.log" 2>&1 || true
    for _ in $(seq 1 100); do
        grep -qE "sde-lock: authentication (failed|error)" "$RUNTIME/lock-d.log" && break
        sleep 0.1
    done
    if grep -qE "sde-lock: authentication (failed|error)" "$RUNTIME/lock-d.log"; then
        echo "  ok: PAM rejected a wrong password"
    else
        echo "  MISSING: PAM did not answer a wrong password"
        fail=1
    fi
    if grep -qF "WindowServer: session unlocked" "$RUNTIME/comp-lock-d.log"; then
        echo "  MISSING: a wrong password unlocked the session"
        fail=1
    else
        echo "  ok: a wrong password leaves the session locked"
    fi

    kill -TERM "$LOCK_D_PID" 2>/dev/null || true
    d_status=0
    wait "$LOCK_D_PID" 2>/dev/null || d_status=$?
    if [[ "$d_status" -eq 0 ]]; then
        echo "  ok: the native locker unlocked and exited cleanly on SIGTERM"
    else
        echo "  MISSING: the native locker exited with status $d_status"
        fail=1
    fi
    check "WindowServer: session unlocked" "$RUNTIME/comp-lock-d.log"
    stop_compositor
    tail -5 "$RUNTIME/lock-d.log" || true
else
    echo "  note: sde-lock is not built; skipping the native locker phase"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> session-lock test FAILED"
    exit 1
fi
echo "==> session-lock test PASSED"
