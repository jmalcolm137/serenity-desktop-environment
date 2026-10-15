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
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

dump_at() { # $1 = scale, $2 = output png, $3 = expected WxH (optional)
    local scale="$1" out="$2" sock="sde-hidpi-$1-$$" expected="${3:-}"
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
    # The client learns the output scale from the compositor, and its first frame can
    # be rendered before that arrives -- so the first dump can be a 1x render. Wait for
    # the dump to reach the size the scale implies rather than taking the first frame.
    if [[ -n "$expected" ]]; then
        for _ in $(seq 1 80); do
            [[ "$(magick identify -format '%wx%h' "$out" 2>/dev/null)" == "$expected" ]] && break
            sleep 0.1
        done
    fi
    pkill -P "$comp" 2>/dev/null || true
    kill "$comp" 2>/dev/null || true
    wait "$comp" 2>/dev/null || true
}

dump_at 1 "$RUNTIME/1x.png"
read -r W1 H1 <<<"$(magick identify -format '%w %h' "$RUNTIME/1x.png" 2>/dev/null || echo '0 0')"
dump_at 2 "$RUNTIME/2x.png" "$((W1 * 2))x$((H1 * 2))"

if [[ ! -s "$RUNTIME/1x.png" || ! -s "$RUNTIME/2x.png" ]]; then
    echo "==> HiDPI fidelity test FAILED: no dump (1x: $(stat -c%s "$RUNTIME/1x.png" 2>/dev/null || echo 0) bytes, 2x: $(stat -c%s "$RUNTIME/2x.png" 2>/dev/null || echo 0) bytes)"
    tail -5 "$RUNTIME/comp-2.log" 2>/dev/null || true
    exit 1
fi

# The compositor has to have applied the scale at all; otherwise the dump sizes below
# say only that the client rendered at 1x twice, which is not a fidelity statement.
if ! grep -qE "output 'HEADLESS-1' [0-9]+x[0-9]+ @2x" "$RUNTIME/comp-2.log" 2>/dev/null; then
    echo "==> HiDPI fidelity test FAILED: the compositor did not report the output at scale 2"
    grep -E "output '" "$RUNTIME/comp-2.log" 2>/dev/null | head -3 || true
    exit 1
fi

# The 2x dump must actually be twice the size of the 1x one; otherwise the scale
# was never applied and the pixel comparison below has nothing to compare.
X1="$(magick identify -format '%w %h' "$RUNTIME/1x.png")"
X2="$(magick identify -format '%w %h' "$RUNTIME/2x.png")"
read -r W1 H1 <<<"$X1"
read -r W2 H2 <<<"$X2"
if [[ "$W2" -ne $((W1 * 2)) || "$H2" -ne $((H1 * 2)) ]]; then
    echo "==> HiDPI fidelity test FAILED: the 2x dump is ${W2}x${H2}, not twice ${W1}x${H1}"
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
