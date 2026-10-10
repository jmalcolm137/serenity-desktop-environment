#!/usr/bin/env bash
# Headless smoke test: run the Serenity Desktop Terminal under the
# libgui-wayland headless compositor and assert that it presents a window backed
# by a live shell.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
HC="${HC:-$LIBGUI_WAYLAND_ROOT/build/xlib-wayland/headless-compositor}"
APP="$BUILD_DIR/bin/Terminal"
FRAME_OUT="${FRAME_OUT:-$BUILD_DIR/terminal-window.png}"

"$SDE_ROOT/scripts/build.sh" Terminal >/dev/null

if [[ ! -x "$HC" ]]; then
    echo "error: headless compositor not found at $HC" >&2
    echo "       run libgui-wayland/scripts/build-headless-compositor.sh" >&2
    exit 1
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-terminal.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

printf 'sleep 5000\n' > "$RUNTIME/input.txt"

export XDG_RUNTIME_DIR="$RUNTIME"
"$HC" --socket sde-terminal --size 1024x768 --timeout 7 \
    --output "$RUNTIME/hc.png" --input "$RUNTIME/input.txt" > "$RUNTIME/hc.log" 2>&1 &
HC_PID=$!
for _ in $(seq 1 100); do [[ -e "$RUNTIME/sde-terminal" ]] && break; sleep 0.1; done

set +e
env XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY=sde-terminal \
    SERENITY_RES_ROOT="$SERENITY_SRC/Base/res" \
    LD_LIBRARY_PATH="$BUILD_DIR/lib" \
    LIBWM_DUMP="$FRAME_OUT" \
    timeout 8 "$APP" > "$RUNTIME/app.log" 2>&1
set -e
wait "$HC_PID" 2>/dev/null || true

failures=0
check() {
    if eval "$2"; then
        printf '%-46s PASS\n' "$1"
    else
        printf '%-46s FAIL\n' "$1"
        failures=$((failures + 1))
    fi
}

check "Terminal window created"  "grep -q \"title='Terminal'\" '$RUNTIME/app.log'"
check "window presented"         "grep -q 'presented window' '$RUNTIME/app.log'"
check "no fatal verification"    "! grep -q 'VERIFICATION FAILED' '$RUNTIME/app.log'"
check "no runtime error"         "! grep -q 'Runtime error' '$RUNTIME/app.log'"
check "frame written"            "[[ -s '$FRAME_OUT' ]]"
check "frame is non-trivial"     "[[ \$(stat -c%s '$FRAME_OUT') -gt 5000 ]]"

if [[ $failures -ne 0 ]]; then
    echo "==> $failures Terminal check(s) failed; app log:"
    cat "$RUNTIME/app.log"
    exit 1
fi
echo "==> Terminal test passed (rendered: $FRAME_OUT)"
