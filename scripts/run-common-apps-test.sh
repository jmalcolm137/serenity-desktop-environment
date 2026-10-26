#!/usr/bin/env bash
# ci-tier: full
# Common desktop-app regression matrix: X11 and Wayland clients must connect,
# map a toplevel, and do so without crashing the client or the compositor.
#
# X11 needs a private network namespace: Xwayland's display names are abstract
# sockets in /tmp/.X11-unix, and a test compositor must never fight a live session
# for them (see run-xwayland-test.sh and docs/26-ci.md §6). The test runs the
# clients that are actually installed; absent optional clients are reported as
# skipped rather than turning the suite red.
set -euo pipefail

if [[ "${SDE_TEST_IN_NAMESPACE:-0}" != 1 ]]; then
    if unshare -Urnm true 2>/dev/null; then
        exec unshare -Urnm env SDE_TEST_IN_NAMESPACE=1 "$0" "$@"
    fi
    if [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/sde" ]]; then
        echo "SKIP: a live session owns the X display and namespaces are unavailable"
        exit 0
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
export MOZ_ENABLE_WAYLAND=1

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$COMP" ]] || { echo "SKIP: WindowServer is not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-common-apps.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/session/portal"
SOCK="sde-common-$$"
DISPLAY_FILE="$RUNTIME/xwayland-display"
COMP_LOG="$RUNTIME/comp.log"
fail=0

cleanup() {
    kill -TERM "${APP_PID:-}" 2>/dev/null || true
    kill -TERM "${COMP_PID:-}" 2>/dev/null || true
    wait "${APP_PID:-}" 2>/dev/null || true
    wait "${COMP_PID:-}" 2>/dev/null || true
    rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

env -u WAYLAND_DISPLAY -u DISPLAY -u SDE_XWAYLAND \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_SESSION_BIN_DIR="$RUNTIME/session-bin" \
    SDE_APP_BIN_DIR="$BUILD/bin" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$COMP_LOG" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 100); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
if [[ ! -e "$XDG_RUNTIME_DIR/$SOCK" ]]; then
    echo "FAIL: compositor did not create $XDG_RUNTIME_DIR/$SOCK" >&2
    tail -20 "$COMP_LOG" >&2 2>/dev/null || true
    exit 1
fi

# Force Xwayland to start even if the first X11 client is missing: the wayland
# test cases below still need a clean compositor.
for _ in $(seq 1 100); do
    [[ -f "$DISPLAY_FILE" ]] && break
    sleep 0.1
done
X11_DISPLAY=""
[[ -f "$DISPLAY_FILE" ]] && X11_DISPLAY="$(cat "$DISPLAY_FILE")"

wait_for() {
    local log="$1" pattern="$2" tries="${3:-80}"
    for _ in $(seq 1 "$tries"); do
        grep -qF "$pattern" "$log" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

require_client() {
    local name="$1"
    if command -v "$name" >/dev/null; then
        return 0
    fi
    if [[ "${CI:-}" == "true" ]]; then
        echo "  FAIL: $name is not installed (scripts/ci-deps.sh must provide it)"
        fail=1
    else
        echo "  SKIP: $name (not installed)"
    fi
    return 1
}

check_client_log() {
    local name="$1" log="$2"
    if grep -qiE 'SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Failed to parse IPC message' "$log"; then
        echo "  FAIL: $name client faulted"
        tail -12 "$log" | sed 's/^/      /'
        fail=1
        return 1
    fi
    return 0
}

run_x11() {
    local name="$1" title="$2"; shift 2
    local log="$RUNTIME/$name.log"
    [[ -n "$X11_DISPLAY" ]] || { echo "  SKIP: $name (Xwayland/DISPLAY unavailable)"; return 0; }
    require_client "$name" || return 0

    env -u WAYLAND_DISPLAY DISPLAY="$X11_DISPLAY" "$@" > "$log" 2>&1 &
    APP_PID=$!
    if wait_for "$COMP_LOG" "X11 window mapped '${title}'" 80; then
        echo "  ok: $name mapped an X11 window ('$title')"
    elif [[ "$name" == "xterm" ]] && grep -qF 'Cannot chown /dev/pts' "$log"; then
        # xterm allocates a PTY and tries to chown it to the tty group. In the
        # rootless user namespace used to isolate Xwayland the tty gid is
        # unmapped, so xterm can exit before mapping even though the same app
        # works in a normal session. Do not turn a harness limitation into an
        # SDE regression.
        echo "  SKIP: $name (PTY ownership is unavailable in the test user namespace)"
    else
        echo "  FAIL: $name did not map an X11 window ('$title')"
        fail=1
    fi
    check_client_log "$name" "$log" || true
    kill -TERM "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
    APP_PID=""
}

run_wayland() {
    local name="$1" title="$2"; shift 2
    local log="$RUNTIME/$name.log"
    require_client "$name" || return 0

    env -u DISPLAY WAYLAND_DISPLAY="$SOCK" GDK_BACKEND=wayland GSK_RENDERER=cairo "$@" > "$log" 2>&1 &
    APP_PID=$!
    if wait_for "$COMP_LOG" "toplevel mapped title='${title}'" 80; then
        echo "  ok: $name mapped a Wayland toplevel ('$title')"
    else
        echo "  FAIL: $name did not map a Wayland toplevel ('$title')"
        fail=1
    fi
    check_client_log "$name" "$log" || true
    kill -TERM "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
    APP_PID=""
}

run_wayland_native() {
    local name="$1" title="$2"; shift 2
    local log="$RUNTIME/$name.log"
    if [[ ! -x "$BUILD/bin/$name" ]]; then
        if [[ "${CI:-}" == "true" ]]; then
            echo "  FAIL: $name is not built"
            fail=1
        else
            echo "  SKIP: $name (not built)"
        fi
        return 0
    fi

    env -u DISPLAY WAYLAND_DISPLAY="$SOCK" \
        SDE_STATE_DIR="$RUNTIME" SDE_SESSION_DIR="$RUNTIME/session" \
        SDE_SESSION_BIN_DIR="$RUNTIME/session-bin" SDE_APP_BIN_DIR="$BUILD/bin" \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" "$@" > "$log" 2>&1 &
    APP_PID=$!
    if wait_for "$COMP_LOG" "toplevel mapped title='${title}'" 80; then
        echo "  ok: $name mapped a native Wayland toplevel"
    else
        echo "  FAIL: $name did not map a native Wayland toplevel"
        fail=1
    fi
    check_client_log "$name" "$log" || true
    kill -TERM "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
    APP_PID=""
}

echo "=== X11 clients ==="
# Xterm is included even though it cannot keep a PTY in the user namespace; the
# compositor contract under test is that the X11 window is created and mapped.
run_x11 xmessage common-xmessage timeout 8 xmessage -name common-xmessage 'hello'
run_x11 xterm common-xterm timeout 8 xterm -title common-xterm -e sh -c 'sleep 6'
run_x11 glxgears glxgears timeout 8 glxgears

echo "=== Wayland clients ==="
run_wayland foot common-foot timeout 8 foot -T common-foot sh -c 'sleep 6'
run_wayland yad common-yad timeout 8 yad --title=common-yad --text=hello --timeout=5
run_wayland zenity common-zenity timeout 10 zenity --info --title=common-zenity --text=hello

# Firefox is the heavyweight common client. Its first window can take several
# seconds to appear, so give it a longer, bounded wait and a private profile.
if require_client firefox; then
    mkdir -p "$RUNTIME/firefox-profile"
    firefox_log="$RUNTIME/firefox.log"
    env -u DISPLAY WAYLAND_DISPLAY="$SOCK" GDK_BACKEND=wayland         MOZ_ENABLE_WAYLAND=1 timeout 20 firefox --no-sandbox --no-remote --new-instance         --profile "$RUNTIME/firefox-profile" about:blank > "$firefox_log" 2>&1 &
    APP_PID=$!
    if wait_for "$COMP_LOG" "toplevel mapped title='Mozilla Firefox'" 180; then
        echo "  ok: firefox mapped a Wayland toplevel ('Mozilla Firefox')"
    else
        echo "  FAIL: firefox did not map a Wayland toplevel"
        fail=1
    fi
    check_client_log firefox "$firefox_log" || true
    kill -TERM "$APP_PID" 2>/dev/null || true
    wait "$APP_PID" 2>/dev/null || true
    APP_PID=""
fi

run_wayland_native TextEditor 'Untitled[*] - Text Editor' timeout 8 "$BUILD/bin/TextEditor"

# The compositor must still be alive after all clients have come and gone.
if ! kill -0 "$COMP_PID" 2>/dev/null; then
    echo "  FAIL: the compositor exited during the app matrix"
    fail=1
else
    echo "  ok: the compositor survived the app matrix"
fi
if grep -qiE 'SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED' "$COMP_LOG"; then
    echo "  FAIL: the compositor log contains a fault"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- client logs (tails) ---"
    for _log in "$RUNTIME"/*.log; do
        [[ -f "$_log" ]] || continue
        echo "--- $(basename "$_log") ---"
        tail -15 "$_log" | sed 's/^/  /'
    done
    echo "--- compositor log (tail) ---"
    tail -40 "$COMP_LOG"
    echo "==> common apps test FAILED"
    exit 1
fi
echo "==> common apps test PASSED"
