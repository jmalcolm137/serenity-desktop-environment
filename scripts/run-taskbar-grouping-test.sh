#!/usr/bin/env bash
# ci-tier: full
# Taskbar window-grouping test.
#
# `[Taskbar] GroupWindows` in WindowServer.ini (default **off**) makes the
# compositor tell the Taskbar to collapse the windows of one application (its app
# id: an xdg app_id / an X11 WM_CLASS) into a single Taskbar button. Off keeps
# SerenityOS's behaviour — one button per window.
#
# This drives a headless compositor + Taskbar + two windows of the same app, for
# a native (xdg app_id) app and an X11 (WM_CLASS) client, and asserts the Taskbar
# only groups them when the option is on. It also checks the app id the port
# reports (the executable base name, replacing the old constant "libwm").
set -euo pipefail

# This test starts an X server, and an X server's sockets are *abstract*
# (@/tmp/.X11-unix/X<n>), so they are shared with every other X server on the
# machine and namespaced by the network namespace rather than the filesystem. Two
# Xwaylands fighting over a display name killed a live session's X server, and a
# missing liveness check in the compositor turned that into a crash
# (docs/26-ci.md §6). Re-exec inside a private network namespace so the test's X
# server cannot be seen by — or collide with — a session's.
if [[ "${SDE_TEST_IN_NAMESPACE:-0}" != 1 ]]; then
    if unshare -Urnm true 2>/dev/null; then
        exec unshare -Urnm env SDE_TEST_IN_NAMESPACE=1 "$0" "$@"
    fi
    # No unprivileged namespaces (some containers): only safe with no session.
    if [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/sde" ]]; then
        echo "SKIP: a live session owns the X display and namespaces are unavailable"
        exit 0
    fi
else
    # A private, correctly-owned X socket directory: Xwayland refuses a
    # /tmp/.X11-unix it does not own, and the host's is owned by root, which is an
    # unmapped (nobody) uid inside a user namespace.
    mkdir -p /tmp/.X11-unix
    mount -t tmpfs -o mode=1777 none /tmp/.X11-unix 2>/dev/null || true
fi

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null 2>&1 || true
ninja -C "$BUILD" TextEditor >/dev/null 2>&1 || true

WIDTH=1280
HEIGHT=720

run_case() {
    local name="$1" group_windows="$2" startup="$3"
    local runtime sock comp_pid
    runtime="$(mktemp -d "${TMPDIR:-/tmp}/sde-group.XXXXXX")"
    chmod 700 "$runtime"
    {
        echo "[Taskbar]"
        echo "GroupWindows=$group_windows"
    } > "$runtime/WindowServer.ini"

    sock="sde-group-$$-$name"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY -u SDE_XWAYLAND \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        SDE_STATE_DIR="$runtime" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" SDE_APP_BIN_DIR="$BUILD/bin" \
        "$COMP" -n "$sock" -s "$startup" \
        > "$runtime/comp.log" 2>&1 &
    comp_pid=$!
    for _ in $(seq 1 80); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    sleep 6
    kill -TERM -- "-$comp_pid" 2>/dev/null || true
    wait "$comp_pid" 2>/dev/null || true
    echo "$runtime/comp.log"
}

TWO_EDITORS="$BUILD/bin/TextEditor /tmp/sde-group-a.txt & $BUILD/bin/TextEditor /tmp/sde-group-b.txt"
OFF_LOG="$(run_case off false "$BUILD/bin/Taskbar & $TWO_EDITORS")"
ON_LOG="$(run_case on true "$BUILD/bin/Taskbar & $TWO_EDITORS")"

echo "=== config + app id (grouping on) ==="
grep -E "taskbar group-windows|app_id='" "$ON_LOG" | head -8 || true

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

# The option is parsed and conveyed to the Taskbar.
check "grouping defaults off (config read as off)" "grep -q 'taskbar group-windows off' \"$OFF_LOG\""
check "grouping on is parsed and conveyed" "grep -q 'taskbar group-windows on' \"$ON_LOG\""

# The port reports a real app id (not the old constant "libwm").
check "native windows report the app id 'TextEditor'" "[[ \$(grep -c \"app_id='TextEditor'\" \"$ON_LOG\") -ge 2 ]]"

# Grouping actually happens only when enabled.
check "grouping off: windows are not grouped" "! grep -q 'joined group' \"$OFF_LOG\""
check "grouping on: the two native windows share a button" "grep -q \"joined group 'TextEditor' (2 windows)\" \"$ON_LOG\""

# X11 clients group by WM_CLASS.
if command -v xterm >/dev/null 2>&1; then
    X_LOG="$(run_case x11 true "$BUILD/bin/Taskbar & xterm & xterm")"
    grep -E "X11 window|joined group" "$X_LOG" | head -8 || true
    check "grouping on: two X11 clients group by WM_CLASS" "grep -q \"joined group 'XTerm' (2 windows)\" \"$X_LOG\""
else
    echo "  note: xterm not found; skipping the X11 (WM_CLASS) case"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- grouping-on log (tail) ---"; tail -40 "$ON_LOG"
    echo "==> taskbar-grouping test FAILED"
    exit 1
fi
echo "==> taskbar-grouping test PASSED"
