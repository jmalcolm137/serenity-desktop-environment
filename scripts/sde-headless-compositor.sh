#!/usr/bin/env bash
# sde-headless-compositor — a drop-in replacement for the xlib-wayland
# `headless-compositor` the port's integration tests used to run.
#
# Same CLI and input-script language, backed by *our* compositor:
#
#   --socket NAME     the Wayland socket to serve in $XDG_RUNTIME_DIR
#   --size WxH        the headless output size
#   --timeout SECONDS how long to stay up (the frame is captured at the end)
#   --output FILE     write the final frame here (PNG)
#   --input FILE      the scripted input, one action per line:
#                       sleep <ms>
#                       motion <x> <y>
#                       button PRESS|RELEASE left|middle|right|<code>
#                       key PRESS|RELEASE <evdev keycode>
#
# The caller starts its client against the socket while this runs, exactly as
# with the xlib compositor. Every input action is delivered through the
# compositor's own virtual-input harness, and the frame through sde-screenshot,
# so the tests exercise the compositor we actually ship.

set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
# The virtual-input harness lives in its own build dir (scripts/build-virtual-input.sh);
# ask that script where it is if it is not on the expected path yet.
VI="${SDE_VIRTUAL_INPUT:-$SDE_ROOT/build/input-test/virtual-input-test}"
SHOT="$SDE_ROOT/build/tools/sde-screenshot"

SOCKET=""
SIZE="1024x768"
TIMEOUT="10"
OUTPUT=""
SCRIPT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --socket) SOCKET="$2"; shift 2 ;;
        --size) SIZE="$2"; shift 2 ;;
        --timeout) TIMEOUT="$2"; shift 2 ;;
        --output) OUTPUT="$2"; shift 2 ;;
        --input) SCRIPT="$2"; shift 2 ;;
        *) shift ;;
    esac
done
[[ -n "$SOCKET" ]] || { echo "sde-headless-compositor: --socket is required" >&2; exit 2; }

WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"
START_SECONDS="$SECONDS"

# A private state directory, but *not* a private XDG_RUNTIME_DIR: the socket has
# to appear where the caller's client looks for it.
STATE="$(mktemp -d "${TMPDIR:-/tmp}/sde-hc.XXXXXX")"
chmod 700 "$STATE"
RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

cleanup() {
    kill "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
    rm -f "$RUNTIME_DIR/$SOCKET" 2>/dev/null || true
    rm -rf "$STATE"
}
COMP_PID=""
trap cleanup EXIT

# The output size is the compositor's [Screens] layout, as everywhere else.
cat > "$STATE/screenlayout.ini" <<EOF
[Screens]
MainScreen=0
[Screen0]
Mode=Device
Device=HEADLESS-1
Left=0
Top=0
Width=$WIDTH
Height=$HEIGHT
ScaleFactor=1
EOF

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$STATE" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_APP_BIN_DIR="$BUILD/bin" \
    "$COMP" -n "$SOCKET" > "$STATE/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 100); do
    [[ -e "$RUNTIME_DIR/$SOCKET" ]] && break
    sleep 0.1
done

if [[ ! -x "$VI" ]]; then
    VI="$("$SDE_ROOT/scripts/build-virtual-input.sh" 2>/dev/null | tail -1 || true)"
fi

# Deliver the scripted input through the compositor's virtual input. Like the
# xlib compositor, wait until a window exists first: an action delivered before
# the client has mapped is a silent no-op, which looks like a client that ignored
# the input. (The scripts also open with a sleep; this is the stronger gate.)
if [[ -n "$SCRIPT" && -f "$SCRIPT" && -x "$VI" ]]; then
    for _ in $(seq 1 60); do
        grep -q 'toplevel mapped' "$STATE/comp.log" 2>/dev/null && break
        sleep 0.1
    done

    args=(--extent "$WIDTH" "$HEIGHT")
    while read -r verb first second; do
        case "$verb" in
            sleep) args+=(--sleep "${first:-10}") ;;
            motion) args+=(--move "${first:-0}" "${second:-0}") ;;
            button)
                case "${second:-}" in
                    left) code=0x110 ;;
                    middle) code=0x112 ;;
                    right) code=0x111 ;;
                    *) code="${second:-0x110}" ;;
                esac
                case "${first:-}" in
                    PRESS) args+=(--button "$code" press) ;;
                    RELEASE) args+=(--button "$code" release) ;;
                esac
                ;;
            key)
                case "${first:-}" in
                    PRESS) args+=(--key "${second:-0}" press) ;;
                    RELEASE) args+=(--key "${second:-0}" release) ;;
                esac
                ;;
        esac
    done < "$SCRIPT"
    env WAYLAND_DISPLAY="$SOCKET" "$VI" "${args[@]}" >/dev/null 2>&1 || true
fi

# Stay up for the caller's timeout, then capture the final frame.
elapsed=$((SECONDS - START_SECONDS))
remaining=$((TIMEOUT - elapsed))
[[ "$remaining" -gt 0 ]] && sleep "$remaining"

if [[ -n "$OUTPUT" ]]; then
    PPM="$STATE/frame.ppm"
    env WAYLAND_DISPLAY="$SOCKET" "$SHOT" "$PPM" >/dev/null 2>&1 || true
    if [[ -s "$PPM" ]]; then
        if command -v magick >/dev/null; then
            magick "$PPM" "$OUTPUT" 2>/dev/null || cp "$PPM" "$OUTPUT"
        else
            cp "$PPM" "$OUTPUT"
        fi
        echo "sde-headless-compositor: captured $OUTPUT"
    else
        echo "sde-headless-compositor: capture failed" >&2
    fi
fi

exit 0
