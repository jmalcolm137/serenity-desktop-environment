#!/usr/bin/env bash
# XWayland test (R5): with [Compositor] XWayland left on (default), an X11 client
# runs and maps a window; with XWayland=false the compositor starts no X server
# (no DISPLAY). Runs headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

command -v xmessage >/dev/null || { echo "SKIP: no xmessage (X11 client)"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

fail=0

run_case() {
    local name="$1" ini="$2"
    local R; R="$(mktemp -d "${TMPDIR:-/tmp}/sde-xwayland-$name.XXXXXX")"
    chmod 700 "$R"
    [[ -n "$ini" ]] && printf '%s\n' "$ini" > "$R/WindowServer.ini"
    local SOCK="sde-xwayland-$name-$$"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        SDE_STATE_DIR="$R" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
    local CP=$!
    for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
    sleep 0.5
    local DISP=""
    [[ -f "$R/xwayland-display" ]] && DISP="$(cat "$R/xwayland-display")"
    if [[ -n "$DISP" ]]; then
        env -u WAYLAND_DISPLAY DISPLAY="$DISP" timeout 6 xmessage -name xwayland-test "hello" >/dev/null 2>&1 &
        sleep 3
    fi
    echo "$R $CP $DISP"
}

echo "=== enabled (default) ==="
read -r R CP DISP <<<"$(run_case on "")"
if grep -q "Xwayland enabled" "$R/comp.log"; then echo "  ok: Xwayland enabled (DISPLAY=$DISP)"; else echo "  FAIL: Xwayland not enabled"; fail=1; fi
if grep -q "Xwayland ready" "$R/comp.log"; then echo "  ok: Xwayland started"; else echo "  FAIL: Xwayland never started"; fail=1; fi
if grep -q "X11 window mapped 'xwayland-test'" "$R/comp.log"; then echo "  ok: the X11 client mapped a window"; else echo "  FAIL: no X11 window mapped"; fail=1; fi
if grep -q "X11 frame 'xwayland-test'" "$R/comp.log"; then echo "  ok: the X11 window got a server-side decoration frame"; else echo "  FAIL: no X11 decoration frame"; fail=1; fi
if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$R/comp.log"; then echo "  FAIL: the compositor faulted"; fail=1; fi
kill "$CP" 2>/dev/null || true; wait "$CP" 2>/dev/null || true
rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR"/sde-xwayland-on-$$* 2>/dev/null || true

echo "=== disabled ([Compositor] XWayland=false) ==="
read -r R CP DISP <<<"$(run_case off "[Compositor]
XWayland=false")"
if grep -q "Xwayland enabled" "$R/comp.log"; then echo "  FAIL: Xwayland enabled despite XWayland=false"; fail=1; else echo "  ok: Xwayland not enabled"; fi
if [[ -f "$R/xwayland-display" ]]; then echo "  FAIL: DISPLAY written despite XWayland=false"; fail=1; else echo "  ok: no X server / DISPLAY"; fi
kill "$CP" 2>/dev/null || true; wait "$CP" 2>/dev/null || true
rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR"/sde-xwayland-off-$$* 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then echo "==> xwayland test FAILED"; exit 1; fi
echo "==> xwayland test PASSED"
