#!/usr/bin/env bash
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

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
ninja -C "$BUILD" Taskbar TextEditor >/dev/null 2>&1 || true

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
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
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
