#!/usr/bin/env bash
# Headless test for xdg-session-management-v1 (R4.2): a client creates a session
# and adds a toplevel; a second client restores that session and the toplevel.
# Asserts the protocol's created/restored events and that geometry is persisted.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/session-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
XDG_SHELL_XML="$WP/stable/xdg-shell/xdg-shell.xml"
SESSION_XML="$SDE_ROOT/WindowServer/protocol/xdg-session-management-v1.xml"
wayland-scanner client-header "$XDG_SHELL_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_SHELL_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
wayland-scanner client-header "$SESSION_XML" "$TOOLS_BUILD/xdg-session-management-v1-client-protocol.h"
wayland-scanner private-code  "$SESSION_XML" "$TOOLS_BUILD/xdg-session-management-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/session-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/xdg-session-management-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/session-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-session-mgmt.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

COMP_PID=""
start_comp() {
    local sock="$1"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless \
        WLR_HEADLESS_OUTPUTS=1 \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm-$sock" \
        SDE_STATE_DIR="$RUNTIME/state" \
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
stop_comp() {
    kill "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
}

echo "== (A) a client creates a session and adds a toplevel =="
start_comp mgmt-a
env WAYLAND_DISPLAY=mgmt-a "$TOOLS_BUILD/session-test" create > "$RUNTIME/create.log" 2>&1 &
CREATE_PID=$!
for _ in $(seq 1 40); do
    grep -q 'SESSION-TEST: created id=' "$RUNTIME/create.log" 2>/dev/null && break
    sleep 0.1
done
# While the client is up, the compositor records its application, so the session
# can relaunch it after a restart (R4.2 "full session restore").
restore_recorded=0
for _ in $(seq 1 20); do
    if grep -qxF 'session-test' "$RUNTIME/state/session-apps" 2>/dev/null; then
        restore_recorded=1
        break
    fi
    sleep 0.1
done
wait "$CREATE_PID" 2>/dev/null || true
cat "$RUNTIME/create.log"
session_id="$(sed -nE 's/.*SESSION-TEST: created id=(.*)/\1/p' "$RUNTIME/create.log" | head -1)"
echo "  session id: '${session_id}'"
stop_comp

echo "== (B) the compositor restarts; a client restores the session =="
start_comp mgmt-b
if [[ -n "$session_id" ]]; then
    env WAYLAND_DISPLAY=mgmt-b "$TOOLS_BUILD/session-test" restore "$session_id" > "$RUNTIME/restore.log" 2>&1 || true
else
    : > "$RUNTIME/restore.log"
fi
cat "$RUNTIME/restore.log"
stop_comp

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

echo "=== session-management test ==="
check "SESSION-TEST: created id=" "$RUNTIME/create.log"
check "SESSION-TEST: restored" "$RUNTIME/restore.log"
check "SESSION-TEST: toplevel-restored" "$RUNTIME/restore.log"

if [[ -n "$session_id" && -s "$RUNTIME/state/sessions/$session_id" ]]; then
    echo "  ok: session state persisted ($(cat "$RUNTIME/state/sessions/$session_id"))"
else
    echo "  MISSING: session state file"
    fail=1
fi

if [[ "$restore_recorded" -eq 1 ]]; then
    echo "  ok: the client's application is recorded for session restore"
else
    echo "  MISSING: running-application record ($RUNTIME/state/session-apps)"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> session-management test FAILED"
    exit 1
fi
echo "==> session-management test PASSED"
