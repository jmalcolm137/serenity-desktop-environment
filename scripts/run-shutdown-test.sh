#!/usr/bin/env bash
# ci-tier: fast
# Clean shutdown: the compositor must tear itself down in order and exit 0.
#
# Every subsystem unhooks its listeners in a finish_*() before the core is
# destroyed (main.cpp's finish()). wlroots asserts that an object's signals have
# no listeners left when it destroys it, so a subsystem that forgets one aborts
# the compositor on the way out, and a destroy handler that runs against a
# half-destroyed compositor crashes it. Before this, the compositor never shut
# down at all: SIGTERM killed it and Alt+Escape did nothing.
#
# Two phases, each with a fresh headless compositor:
#   (A) with every kind of client connected (the Taskbar's layer surface, an xdg
#       window, an X11 window, virtual keyboard/pointer/touch/tablet devices and a
#       held session lock), SIGTERM must end it with status 0;
#   (B) Alt+Escape must quit it, also with status 0.
set -euo pipefail

# The X11 client needs an X server, whose sockets are abstract and shared across
# the machine; keep the test's own in a private network namespace (see
# run-xwayland-exit-test.sh).
if [[ "${SDE_TEST_IN_NAMESPACE:-0}" != 1 ]]; then
    if unshare -Urnm true 2>/dev/null; then
        exec unshare -Urnm env SDE_TEST_IN_NAMESPACE=1 "$0" "$@"
    fi
    if [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/sde" ]]; then
        # A live session owns the X display: run without the X11 client.
        export SDE_SHUTDOWN_NO_X11=1
    fi
else
    mkdir -p /tmp/.X11-unix
    mount -t tmpfs -o mode=1777 none /tmp/.X11-unix 2>/dev/null || true
fi

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
INPUT_BUILD="$(dirname "$("$SDE_ROOT/scripts/build-virtual-input.sh" | tail -1)")"
VI="$INPUT_BUILD/virtual-input-test"
APP="$INPUT_BUILD/native-app-test"

# The session-lock client, built as run-lock-test.sh does.
LOCK_BUILD="$SDE_ROOT/build/shutdown-test"
mkdir -p "$LOCK_BUILD"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
wayland-scanner client-header "$WP/staging/ext-session-lock/ext-session-lock-v1.xml" "$LOCK_BUILD/ext-session-lock-v1-client-protocol.h"
wayland-scanner private-code "$WP/staging/ext-session-lock/ext-session-lock-v1.xml" "$LOCK_BUILD/ext-session-lock-v1-protocol.c"
cc -O2 -I "$LOCK_BUILD" "$SDE_ROOT/Tools/lock-test.c" "$LOCK_BUILD/ext-session-lock-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) -o "$LOCK_BUILD/lock-test"
LOCK="$LOCK_BUILD/lock-test"

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-shutdown.XXXXXX")"
chmod 700 "$R"
CP=""
CLIENTS=()
cleanup() {
    for pid in "${CLIENTS[@]}"; do kill "$pid" 2>/dev/null || true; done
    [[ -n "$CP" ]] && kill -KILL "$CP" 2>/dev/null || true
    rm -rf "$R"
    rm -f "$XDG_RUNTIME_DIR"/sde-shutdown-$$* 2>/dev/null || true
}
trap cleanup EXIT

fail=0
ok() { echo "  ok: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }

start_compositor() {
    local name="$1" startup="$2"
    SOCK="sde-shutdown-$$-$name"
    LOG="$R/$name.log"
    mkdir -p "$R/$name"
    local xwayland=()
    [[ "${SDE_SHUTDOWN_NO_X11:-0}" == 1 ]] && xwayland=(SDE_XWAYLAND=0)
    env -u WAYLAND_DISPLAY -u DISPLAY -u SDE_XWAYLAND "${xwayland[@]}" \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        SDE_STATE_DIR="$R/$name" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" SDE_APP_BIN_DIR="$BUILD/bin" \
        "$COMP" -n "$SOCK" ${startup:+-s "$startup"} > "$LOG" 2>&1 &
    CP=$!
    for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
}

# Waits up to $1 seconds for the compositor to exit and sets STATUS to its exit
# status ("hung" if it had to be killed). Not run in a subshell: only the shell
# that started the compositor can wait for it.
wait_for_exit() {
    local seconds="$1"
    for _ in $(seq 1 $((seconds * 10))); do
        kill -0 "$CP" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "$CP" 2>/dev/null; then
        kill -KILL "$CP" 2>/dev/null || true
        wait "$CP" 2>/dev/null || true
        STATUS=hung
        return
    fi
    STATUS=0
    wait "$CP" || STATUS=$?
}

check_clean_exit() {
    local what="$1" status="$2"
    if [[ "$status" == 0 ]]; then
        ok "$what: the compositor exited with status 0"
    else
        bad "$what: the compositor exited with status $status"
    fi
    if grep -q "WindowServer: shut down" "$LOG"; then
        ok "$what: the teardown ran to the end"
    else
        bad "$what: the teardown did not finish"
    fi
    if grep -aqiE "Assertion|wl_list_empty|listener_list|SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$LOG"; then
        bad "$what: an assertion or fault during teardown:"
        grep -aiE "Assertion|wl_list_empty|listener_list|SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$LOG" | head -3
    else
        ok "$what: no wlroots assertion or fault"
    fi
}

echo "=== (A) SIGTERM with every kind of client connected ==="
start_compositor busy "$BUILD/bin/Taskbar"
env WAYLAND_DISPLAY="$SOCK" "$APP" --title shutdown-window --linger 30000 >/dev/null 2>&1 &
CLIENTS+=($!)
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --move 200 200 --touch 300 300 \
    --tablet-proximity 400 400 --tablet-tip-down --linger 30000 >/dev/null 2>&1 &
CLIENTS+=($!)
if [[ "${SDE_SHUTDOWN_NO_X11:-0}" != 1 ]] && command -v xmessage >/dev/null; then
    for _ in $(seq 1 50); do [[ -f "$R/busy/xwayland-display" ]] && break; sleep 0.1; done
    if [[ -f "$R/busy/xwayland-display" ]]; then
        env -u WAYLAND_DISPLAY DISPLAY="$(cat "$R/busy/xwayland-display")" \
            timeout 30 xmessage -name shutdown-x11 "x" >/dev/null 2>&1 &
        CLIENTS+=($!)
    fi
else
    echo "  note: no X11 client (a live session owns the X display, or xmessage is missing)"
fi
for _ in $(seq 1 50); do grep -q "toplevel mapped title='shutdown-window'" "$LOG" && break; sleep 0.1; done
grep -q "toplevel mapped title='shutdown-window'" "$LOG" && ok "an xdg window mapped" || bad "the xdg window never mapped"
if [[ "${#CLIENTS[@]}" -ge 3 ]]; then
    for _ in $(seq 1 50); do grep -q "X11 window mapped 'shutdown-x11'" "$LOG" && break; sleep 0.1; done
    grep -q "X11 window mapped 'shutdown-x11'" "$LOG" && ok "an X11 window mapped" || bad "the X11 window never mapped"
fi
env WAYLAND_DISPLAY="$SOCK" "$LOCK" --hold 30000 --no-unlock > "$R/lock.log" 2>&1 &
CLIENTS+=($!)
for _ in $(seq 1 50); do grep -qi "locked" "$R/lock.log" && break; sleep 0.1; done
grep -qi "locked" "$R/lock.log" && ok "the session is locked" || bad "the lock client never locked"
sleep 0.5
kill -0 "$CP" 2>/dev/null || bad "the compositor died before the shutdown"
kill -TERM "$CP"
wait_for_exit 10
check_clean_exit "SIGTERM" "$STATUS"
for pid in "${CLIENTS[@]}"; do kill "$pid" 2>/dev/null || true; done
CLIENTS=()

echo "=== the state directory after the exit ==="
# The compositor is not gone the instant it is signalled: it disconnects its
# clients and runs every subsystem's teardown first, and that work touches the
# state directory (the clients' own teardown handlers among them). A caller that
# removes the directory the moment it signals the process is racing that work --
# it fails with "Directory not empty" -- so it must wait for the exit. The test
# scripts do; this checks that the wait is also sufficient.
if rm -rf "$R/busy"; then
    ok "the state directory is removable once the compositor has exited"
else
    bad "the state directory could not be removed after the exit"
fi

echo "=== (B) Alt+Escape ==="
start_compositor quit ""
sleep 0.3
# evdev: KEY_LEFTALT 56, KEY_ESC 1.
env WAYLAND_DISPLAY="$SOCK" "$VI" --key 56 press --key 1 press --key 1 release --key 56 release >/dev/null 2>&1 || true
wait_for_exit 10
check_clean_exit "Alt+Escape" "$STATUS"
CP=""

if [[ "$fail" -ne 0 ]]; then
    echo "--- (A) compositor log (tail) ---"; tail -20 "$R/busy.log" 2>/dev/null || true
    echo "==> shutdown test FAILED"
    exit 1
fi
echo "==> shutdown test PASSED"
