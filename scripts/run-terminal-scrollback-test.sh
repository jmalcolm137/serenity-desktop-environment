#!/usr/bin/env bash
# Terminal scrollback test (R5): scrolling back with the wheel must move the
# viewport, and output that arrives while scrolled back must not yank it back to
# the bottom. This guards the "don't repaint the whole window while scrolled"
# optimisation in TerminalWidget::flush_dirty_lines/terminal_history_changed.
#
# Runs headless: starts the SDE compositor, runs Terminal with a shell that
# prints a long scrollback (and more output later), wheels up, and compares
# screenshots.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TERMINAL="$BUILD/bin/Terminal"
SHOT="$SDE_ROOT/build/tools/sde-screenshot"
TOOLS_BUILD="$SDE_ROOT/build/terminal-scrollback-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
"$SDE_ROOT/scripts/build.sh" Terminal >/dev/null

if ! command -v compare >/dev/null 2>&1; then
    echo "note: ImageMagick compare not found; skipping the scrollback test"
    echo "==> terminal scrollback test SKIPPED"
    exit 0
fi

mkdir -p "$TOOLS_BUILD"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
wayland-scanner client-header "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
wayland-scanner client-header "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-tscroll.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

# Print a long scrollback, then more lines later, then stay alive. Each line
# has a varying-length bar so scrolling actually changes pixels.
cat > "$RUNTIME/print.sh" <<'EOF'
#!/bin/sh
i=0
while [ "$i" -lt 400 ]; do
    i=$((i + 1))
    bar=$(printf '%*s' $((i % 37 + 1)) '' | tr ' ' '=')
    printf 'scrollback line %04d %s\n' "$i" "$bar"
done
sleep 6
i=400
while [ "$i" -lt 420 ]; do
    i=$((i + 1))
    bar=$(printf '%*s' $((i % 23 + 1)) '' | tr ' ' '+')
    printf 'LATE OUTPUT %04d %s\n' "$i" "$bar"
done
sleep 30
EOF
chmod +x "$RUNTIME/print.sh"

SOCK="sde-tscroll-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
trap 'kill "$COMP_PID" 2>/dev/null || true; for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.4

env WAYLAND_DISPLAY="$SOCK" SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" "$TERMINAL" -e "$RUNTIME/print.sh" > "$RUNTIME/terminal.log" 2>&1 &
TERM_PID=$!
sleep 3
if ! kill -0 "$TERM_PID" 2>/dev/null; then
    echo "FAIL: Terminal exited early ($(head -1 "$RUNTIME/terminal.log"))"
    exit 1
fi

shot() { WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" "$SHOT" "$1" >/dev/null 2>&1; }
shot "$RUNTIME/bottom.ppm"

# Scroll up (no mouse tracking is enabled, so the wheel drives the scrollback).
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" --extent 1280 720 --move 300 300 >/dev/null 2>&1 || true
sleep 0.3
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" --extent 1280 720 --axis 0 -1 --axis 0 -1 --axis 0 -1 --axis 0 -1 >/dev/null 2>&1 || true
sleep 0.8
shot "$RUNTIME/scrolled.ppm"

# The shell prints more lines while we are scrolled back; the viewport must not
# jump to the bottom.
sleep 4
shot "$RUNTIME/scrolled_later.ppm"

kill "$TERM_PID" 2>/dev/null || true
wait "$TERM_PID" 2>/dev/null || true

diff_metric() { compare -metric AE "$1" "$2" null: 2>&1 | awk '{print $1+0}' || true; }
moved="$(diff_metric "$RUNTIME/bottom.ppm" "$RUNTIME/scrolled.ppm")"
jumped="$(diff_metric "$RUNTIME/scrolled.ppm" "$RUNTIME/scrolled_later.ppm")"
echo "bottom -> scrolled diff: $moved"
echo "scrolled -> after more output diff: $jumped"

fail=0
if awk "BEGIN{exit !($moved > 800 && $moved > $jumped * 5)}"; then
    echo "  ok: the wheel scrolled the viewport ($moved vs $jumped)"
else
    echo "  FAIL: the wheel did not scroll the viewport ($moved vs $jumped)"
    fail=1
fi
if awk "BEGIN{exit !($jumped < 500)}"; then
    echo "  ok: output while scrolled back did not yank the viewport"
else
    echo "  FAIL: the viewport moved when more output arrived ($jumped)"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> terminal scrollback test FAILED"
    exit 1
fi
echo "==> terminal scrollback test PASSED"
