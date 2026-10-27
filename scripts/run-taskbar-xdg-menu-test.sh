#!/usr/bin/env bash
# Taskbar XDG menu test.
#
# Generates .desktop entries for the Serenity apps (.af -> .desktop), starts the
# Taskbar headless with WindowServer.ini [Taskbar] XDGMenu=true, and asserts the
# menu was built from the entries; then repeats with XDGMenu=false and asserts the
# Serenity .af menu is used. Runs entirely headless and in a private runtime, so
# it never touches a live session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

if [[ "${SDE_TEST_NO_BUILD:-0}" != "1" ]]; then
    "$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
    "$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null
fi

[[ -x "$BUILD/bin/Taskbar" ]] || { echo "SKIP: Taskbar not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-taskbar-xdg.XXXXXX")"
chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME/run"
export SDE_STATE_DIR="$RUNTIME/state"
export SDE_SESSION_DIR="$RUNTIME/session"
export SDE_WM_PORTAL_PATH="$RUNTIME/wm"
mkdir -p "$XDG_RUNTIME_DIR" "$SDE_STATE_DIR" "$SDE_SESSION_DIR/portal" "$SDE_SESSION_DIR/sbin" "$RUNTIME/data-home"

XDG_DATA="$RUNTIME/xdg-data"
"$SDE_ROOT/scripts/generate-desktop-entries.sh" --source "$SERENITY/Base/res/apps" --output "$XDG_DATA" >/dev/null
entry_count="$(find "$XDG_DATA/applications" -name '*.desktop' | wc -l)"
echo "  note: generated $entry_count .desktop entries"

COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

run_taskbar() {
    local label="$1" menu="$2" sock="$3"
    cat > "$SDE_STATE_DIR/WindowServer.ini" <<EOF
[Taskbar]
XDGMenu=$menu
EOF
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_XWAYLAND=0 \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SDE_APP_BIN_DIR="$BUILD/bin" \
        SDE_SESSION_BIN_DIR="$SDE_SESSION_DIR/sbin" \
        XDG_DATA_DIRS="$XDG_DATA" \
        XDG_DATA_HOME="$RUNTIME/data-home" \
        "$COMP" -n "$sock" -s "$BUILD/bin/Taskbar" > "$RUNTIME/comp-$label.log" 2>&1 &
    COMP_PID=$!
    for _ in $(seq 1 300); do
        grep -q "layer surface namespace='serenity-taskbar'" "$RUNTIME/comp-$label.log" 2>/dev/null && break
        sleep 0.1
    done
    sleep 1
    kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    sleep 0.3
    kill -KILL -- "-$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    COMP_PID=""
}

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

echo "=== XDG menu enabled ==="
run_taskbar xdg true "sde-taskbar-xdg-$$"
check "Taskbar: using XDG menu" "$RUNTIME/comp-xdg.log"
check "Taskbar: XDG menu discovered" "$RUNTIME/comp-xdg.log"
check "Taskbar: XDG app 'Calculator' (/bin/Calculator)" "$RUNTIME/comp-xdg.log"
check "Taskbar: XDG menu added" "$RUNTIME/comp-xdg.log"
if grep -qF "Taskbar: using Serenity menu" "$RUNTIME/comp-xdg.log"; then
    echo "  UNEXPECTED: the Serenity menu was used with XDGMenu=true"
    fail=1
fi

echo "=== Serenity menu (default) ==="
run_taskbar ser false "sde-taskbar-ser-$$"
check "Taskbar: using Serenity menu" "$RUNTIME/comp-ser.log"
check "system menu discovered" "$RUNTIME/comp-ser.log"
if grep -qF "Taskbar: using XDG menu" "$RUNTIME/comp-ser.log"; then
    echo "  UNEXPECTED: the XDG menu was used with XDGMenu=false"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> taskbar XDG menu test FAILED"
    echo "--- XDG compositor log ---"; grep -E "Taskbar:" "$RUNTIME/comp-xdg.log" | head -20
    echo "--- Serenity compositor log ---"; grep -E "Taskbar:" "$RUNTIME/comp-ser.log" | head -20
    exit 1
fi
echo "==> taskbar XDG menu test PASSED"
