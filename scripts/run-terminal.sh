#!/usr/bin/env bash
# Headless smoke test: run the Serenity Terminal under the SDE compositor's
# headless backend and assert that it presents a window backed by a live shell.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
APP="$BUILD_DIR/bin/Terminal"
FRAME_OUT="${FRAME_OUT:-$SDE_ROOT/build/terminal-window.png}"

"$SDE_ROOT/scripts/build.sh" Terminal >/dev/null
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-terminal.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-terminal-$$"
rm -f "$FRAME_OUT"

# Our own compositor's headless backend hosts the Terminal; no host compositor
# and no third-party test compositor are involved.
env -u WAYLAND_DISPLAY -u DISPLAY \
    XDG_RUNTIME_DIR="$RUNTIME" \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib" \
    LIBWM_DUMP="$FRAME_OUT" \
    "$COMP" -n "$SOCK" -s "$APP" > "$RUNTIME/session.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 100); do [[ -e "$RUNTIME/$SOCK" ]] && break; sleep 0.1; done
# Give the Terminal time to map, present, and dump its frame.
for _ in $(seq 1 80); do [[ -s "$FRAME_OUT" ]] && break; sleep 0.1; done

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

failures=0
check() {
    if eval "$2"; then
        printf '%-46s PASS\n' "$1"
    else
        printf '%-46s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "Terminal window created"  "grep -q \"title='Terminal'\" '$RUNTIME/session.log'"
check "window presented"         "grep -q 'presented window' '$RUNTIME/session.log'"
check "no fatal verification"    "! grep -q 'VERIFICATION FAILED' '$RUNTIME/session.log'"
check "no runtime error"         "! grep -q 'Runtime error' '$RUNTIME/session.log'"
check "frame written"            "[[ -s '$FRAME_OUT' ]]"
check "frame is non-trivial"     "[[ \$(stat -c%s '$FRAME_OUT') -gt 2000 ]]"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures Terminal check(s) failed; session log:"
    cat "$RUNTIME/session.log"
    exit 1
fi
echo "==> Terminal test passed (rendered: $FRAME_OUT)"
