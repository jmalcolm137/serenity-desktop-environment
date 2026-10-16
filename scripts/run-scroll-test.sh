#!/usr/bin/env bash
# Wheel-scroll regression test (R5/terminal scrollback): a LibGUI scrollable
# widget must scroll when the compositor delivers fractional Wayland axis
# deltas (trackpads / high-resolution wheels), which used to be truncated to 0.
#
# Runs headless: it starts the SDE compositor with the headless backend, opens
# TextEditor on a long file, injects many small (fractional) axis events through
# the wlroots virtual-pointer protocol, and asserts the rendered content
# changed. Without the fix (each delta truncated to 0) the screenshots match.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SHOT="$SDE_ROOT/build/tools/sde-screenshot"
TOOLS_BUILD="$SDE_ROOT/build/scroll-test"
EDITOR="$BUILD/bin/TextEditor"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if [[ ! -x "$EDITOR" ]]; then
    echo "note: TextEditor not built; skipping the scroll test"
    echo "==> scroll test SKIPPED"
    exit 0
fi
if ! command -v compare >/dev/null 2>&1; then
    echo "note: ImageMagick compare not found; skipping the scroll test"
    echo "==> scroll test SKIPPED"
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
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$TOOLS_BUILD/virtual-input-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-scroll.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

SOCK="sde-scroll-$$"
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

# A long, line-distinct file so scrolling visibly changes the rendering.
seq -f 'scroll-test line %04g --------------------' 1 400 > "$RUNTIME/long.txt"

env WAYLAND_DISPLAY="$SOCK" SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" "$EDITOR" "$RUNTIME/long.txt" > "$RUNTIME/editor.log" 2>&1 &
EDITOR_PID=$!
sleep 3
if ! kill -0 "$EDITOR_PID" 2>/dev/null; then
    echo "FAIL: TextEditor exited early ($(head -1 "$RUNTIME/editor.log"))"
    exit 1
fi

shot() { WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" "$SHOT" "$1" >/dev/null 2>&1; }
shot "$RUNTIME/before.ppm"
sleep 0.5
shot "$RUNTIME/before2.ppm"

# Move the pointer into the editor first (a separate round-trip, as in real
# use), then send many *fractional* axis deltas (0.4 each) that must accumulate
# to whole scroll steps.
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" --extent 1280 720 --move 300 300 >/dev/null 2>&1 || true
sleep 0.4
args=()
for _ in $(seq 1 12); do args+=(--axis 0 0.4); done
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/virtual-input-test" --extent 1280 720 "${args[@]}" > "$RUNTIME/input.log" 2>&1 || true
sleep 1

shot "$RUNTIME/after.ppm"
kill "$EDITOR_PID" 2>/dev/null || true
wait "$EDITOR_PID" 2>/dev/null || true

# A cursor blink changes a handful of pixels; scrolling changes many.
# `compare` prints "<value> (<normalized>)" and exits non-zero when the images
# differ, so swallow its status; take the first token.
diff_metric() { compare -metric AE "$1" "$2" null: 2>&1 | awk '{print $1+0}' || true; }
idle="$(diff_metric "$RUNTIME/before.ppm" "$RUNTIME/before2.ppm")"
scrolled="$(diff_metric "$RUNTIME/before.ppm" "$RUNTIME/after.ppm")"
echo "idle pixel diff:     $idle"
echo "scrolled pixel diff: $scrolled"

fail=0
if awk "BEGIN{exit !($scrolled > 2000)}"; then
    echo "  ok: fractional scroll deltas moved the content ($scrolled)"
else
    echo "  FAIL: content did not scroll ($scrolled)"
    fail=1
fi
if awk "BEGIN{exit !($scrolled > $idle * 10)}"; then
    echo "  ok: the change is from scrolling, not idle repaint"
else
    echo "  FAIL: idle repaint ($idle) is comparable to the scroll change ($scrolled)"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> scroll test FAILED"
    exit 1
fi
echo "==> scroll test PASSED"
