#!/usr/bin/env bash
# Regression test for a replaced login leaving its compositor behind.
#
# A display manager can start a new session while the old one is still "closing"
# (logind keeps it alive while any process remains). The old WindowServer then
# still owns the seat's Wayland socket. The new compositor used to fall back to a
# generated wayland-N socket, so the session's exported WAYLAND_DISPLAY pointed
# at the stale server; screenshot tools against that server reported
# "no wl_output" even though the desktop was running.
#
# Covers:
#   (A) an explicit -n socket that is already in use makes the new compositor
#       fail instead of silently running on a generated socket;
#   (B) sde-session.sh stops the stale same-seat compositor, reclaims the display
#       name, and the new compositor advertises wl_output on it.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SDE="$SDE_ROOT/scripts/sde-session.sh"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-stale-session.XXXXXX")"
export XDG_RUNTIME_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'for pid in "${SUP_PID:-}" "${OLD_PID:-}" "${NEW_PID:-}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; done; for _ in $(seq 1 60); do kill -0 "${SUP_PID:-${OLD_PID:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT

cc -O2 "$SDE_ROOT/Tools/testing/wayland-globals-test.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$RUNTIME/wayland-globals-test"

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
check_globals() {
    if grep -qE " wl_output v[0-9]+" "$1"; then
        echo "  ok: wl_output is advertised"
    else
        echo "  MISSING: wl_output in $1"
        cat "$1" | sed 's/^/    /'
        fail=1
    fi
    return 0
}
wait_for_socket() {
    local socket="$1"
    for _ in $(seq 1 100); do
        [ -S "$socket" ] && return 0
        sleep 0.05
    done
    return 1
}

sock="sde-stale-$$"
socket="$RUNTIME/$sock"

echo "== (A) an explicit socket that is in use is not silently replaced =="
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME/state-old" \
    LIBWM_WM_PORTAL_PATH="$RUNTIME/wm-old" \
    "$COMP" -n "$sock" > "$RUNTIME/old.log" 2>&1 &
OLD_PID=$!
wait_for_socket "$socket"
for _ in $(seq 1 80); do
    grep -qF "output 'HEADLESS-1'" "$RUNTIME/old.log" 2>/dev/null && break
    sleep 0.05
done
env WAYLAND_DISPLAY="$sock" "$RUNTIME/wayland-globals-test" > "$RUNTIME/old-globals.log" 2>&1 || true
check_globals "$RUNTIME/old-globals.log"

if env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME/state-duplicate" \
    LIBWM_WM_PORTAL_PATH="$RUNTIME/wm-duplicate" \
    "$COMP" -n "$sock" > "$RUNTIME/duplicate.log" 2>&1; then
    echo "  FAIL: a second compositor started on the occupied socket"
    fail=1
else
    echo "  ok: the second compositor refused the occupied socket"
fi
check "could not add socket '$sock'" "$RUNTIME/duplicate.log"
if grep -qF "running on WAYLAND_DISPLAY=wayland-" "$RUNTIME/duplicate.log"; then
    echo "  FAIL: the second compositor fell back to a generated socket"
    fail=1
else
    echo "  ok: no wayland-N fallback"
fi

echo "== (B) the session reclaims the display and advertises wl_output =="
env -u WAYLAND_DISPLAY -u DISPLAY \
    SDE_SESSION_DISPLAY="$sock" \
    SDE_SESSION_COMPOSITOR="$COMP -n $sock -s true" \
    SDE_SESSION_SHELL="true" \
    SDE_SESSION_STARTUP_APPS= \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_RESTORE=0 \
    SDE_SESSION_MAX_RESTARTS=0 \
    SDE_SESSION_RESTART_DELAY=0 \
    SDE_SESSION_IDLE_SECONDS=0 \
    SDE_SESSION_WATCHDOG_SECONDS=0 \
    SDE_STATE_DIR="$RUNTIME/state-new" \
    LIBWM_WM_PORTAL_PATH="$RUNTIME/wm-new" \
    "$SDE" > "$RUNTIME/session.log" 2>&1 &
SUP_PID=$!
for _ in $(seq 1 150); do
    grep -qF "supervisor ready" "$RUNTIME/session.log" 2>/dev/null && break
    sleep 0.05
done
wait_for_socket "$socket"

old_gone=0
for _ in $(seq 1 80); do
    state="$(ps -o stat= -p "$OLD_PID" 2>/dev/null || true)"
    case "$state" in ''|Z*) old_gone=1; break ;; esac
    sleep 0.05
done
[ "$old_gone" = 1 ] && wait "$OLD_PID" 2>/dev/null || true
if kill -0 "$OLD_PID" 2>/dev/null; then
    echo "  FAIL: the stale compositor is still running"
    fail=1
else
    echo "  ok: the stale compositor is gone"
fi

for _ in $(seq 1 100); do
    env WAYLAND_DISPLAY="$sock" "$RUNTIME/wayland-globals-test" > "$RUNTIME/new-globals.log" 2>&1 || true
    grep -qE " wl_output v[0-9]+" "$RUNTIME/new-globals.log" && break
    sleep 0.05
done
check_globals "$RUNTIME/new-globals.log"
check "output 'HEADLESS-1'" "$RUNTIME/session.log"
check "stopping stale compositor $OLD_PID on display '$sock'" "$RUNTIME/session.log"

kill -TERM "$SUP_PID" 2>/dev/null || true
wait "$SUP_PID" 2>/dev/null || true
SUP_PID=""

if [[ "$fail" -ne 0 ]]; then
    echo "==> stale-session test FAILED"
    exit 1
fi
echo "==> stale-session test PASSED"
