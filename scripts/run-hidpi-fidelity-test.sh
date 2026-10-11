#!/usr/bin/env bash
# HiDPI fidelity test: render a LibWindowServer app at 1x and 2x under the
# headless backend and assert the 2x backing store is the 1x one pixel-doubled
# (nearest). A faithful 2x render must be *identical* to 1x upscaled 2x, so
# "graphics are 2x but otherwise look like 1x".
#
# We dump the client's backing store (LIBWM_DUMP) at both scales, downscale the
# 2x dump by 2 (nearest), and compare to the 1x dump. The metric is the fraction
# of differing pixels (normalized AE); the test fails if it exceeds
# SDE_HIDPI_MAX_DIFF (default 0.02 = 2%).
#
# Env: SDE_HIDPI_APP (default TextEditor), SDE_HIDPI_MAX_DIFF.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
APP="${SDE_HIDPI_APP:-$BUILD/bin/TextEditor}"
MAX_DIFF="${SDE_HIDPI_MAX_DIFF:-0.02}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-hidpi.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

dump_at() { # $1 = scale, $2 = output png
    local scale="$1" out="$2" sock="sde-hidpi-$1-$$"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" "$out"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        SDE_OUTPUT_SCALE="$scale" \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm-$scale" \
        SERENITY_RES_ROOT="$SERENITY/Base/res" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        LIBWM_DUMP="$out" \
        "$COMP" -n "$sock" -s "$APP" > "$RUNTIME/comp-$scale.log" 2>&1 &
    local comp=$!
    for _ in $(seq 1 60); do [[ -s "$out" ]] && break; sleep 0.1; done
    pkill -P "$comp" 2>/dev/null || true
    kill "$comp" 2>/dev/null || true
    wait "$comp" 2>/dev/null || true
}

dump_at 1 "$RUNTIME/1x.png"
dump_at 2 "$RUNTIME/2x.png"

if [[ ! -s "$RUNTIME/1x.png" || ! -s "$RUNTIME/2x.png" ]]; then
    echo "==> HiDPI fidelity test FAILED: no dump (1x: $(stat -c%s "$RUNTIME/1x.png" 2>/dev/null || echo 0) bytes, 2x: $(stat -c%s "$RUNTIME/2x.png" 2>/dev/null || echo 0) bytes)"
    tail -5 "$RUNTIME/comp-2.log" 2>/dev/null || true
    exit 1
fi

# 2x downscaled by 2 (nearest) should equal 1x.
magick "$RUNTIME/2x.png" -filter point -resize 50% "$RUNTIME/2x-down.png"
# magick compare exits non-zero when the images differ, so tolerate that here.
compare_out="$(magick compare -metric AE "$RUNTIME/1x.png" "$RUNTIME/2x-down.png" null: 2>&1 || true)"
actual="$(printf '%s' "$compare_out" | sed -nE 's/.*\(([0-9.eE+-]+)\).*/\1/p')"
[[ -n "$actual" ]] || actual="0"

printf '1x: %s   2x: %s\n' \
    "$(magick identify -format '%wx%h' "$RUNTIME/1x.png")" \
    "$(magick identify -format '%wx%h' "$RUNTIME/2x.png")"
printf 'differing-pixel fraction (2x-down vs 1x): %s  (max %s)\n' "$actual" "$MAX_DIFF"

if awk "BEGIN { exit !($actual <= $MAX_DIFF) }"; then
    echo "==> HiDPI fidelity test PASSED"
else
    echo "==> HiDPI fidelity test FAILED ($actual > $MAX_DIFF)"
    magick compare "$RUNTIME/1x.png" "$RUNTIME/2x-down.png" -compose difference "$RUNTIME/diff.png" 2>/dev/null || true
    echo "    diff image: $RUNTIME/diff.png"
    exit 1
fi
