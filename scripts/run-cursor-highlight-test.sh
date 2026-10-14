#!/usr/bin/env bash
# Super+H toggles the cursor highlight (R3, docs/04).
#
# SerenityOS's WindowManager binds Super+H to the cursor highlight. SDE's
# highlight (R4.7) is otherwise driven by cursor.ini [Highlight], so Super+H
# toggles it at runtime (using SerenityOS's default radius/colour when the
# settings have not configured one).
#
# This drives a headless compositor: capture the output, press Super+H (the
# highlight must appear), press it again (it must go away), asserting both the
# compositor log and the pixels. Super+H must not be taken as a Super tap.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

SHOT="$SDE_ROOT/build/tools/sde-screenshot"
if ! command -v compare >/dev/null 2>&1; then
    echo "==> cursor-highlight test SKIPPED (ImageMagick compare not found)"
    exit 0
fi

WIDTH="${SDE_HIGHLIGHT_WIDTH:-800}"
HEIGHT="${SDE_HIGHLIGHT_HEIGHT:-600}"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-highlight.XXXXXX")"
chmod 700 "$RUNTIME"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

SOCK="sde-highlight-$$"
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    WLR_HEADLESS_OUTPUT_SIZE="${WIDTH}x${HEIGHT}" \
    SDE_STATE_DIR="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" SDE_APP_BIN_DIR="$BUILD/bin" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 2

capture() {
    env WAYLAND_DISPLAY="$SOCK" "$SHOT" "$1" >/dev/null 2>&1
}

# Park the pointer in the middle so the highlight is well inside the output.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --move $((WIDTH / 2)) $((HEIGHT / 2)) --linger 500 >/dev/null 2>&1
sleep 0.5
capture "$RUNTIME/before.ppm"

# Super+H -> the highlight appears.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --key 125 press --sleep 60 --key 35 press --sleep 60 --key 35 release --sleep 60 --key 125 release --linger 600 >/dev/null 2>&1
sleep 0.6
capture "$RUNTIME/during.ppm"

# Super+H again -> it goes away.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --key 125 press --sleep 60 --key 35 press --sleep 60 --key 35 release --sleep 60 --key 125 release --linger 600 >/dev/null 2>&1
sleep 0.6
capture "$RUNTIME/after.ppm"

diff_on="$(compare -metric AE "$RUNTIME/before.ppm" "$RUNTIME/during.ppm" null: 2>&1 | awk '{print $1}' || true)"
diff_off="$(compare -metric AE "$RUNTIME/before.ppm" "$RUNTIME/after.ppm" null: 2>&1 | awk '{print $1}' || true)"
# The metric is a float; fold it into 0/1 flags for the checks below.
drawn="$(awk -v d="${diff_on:-0}" 'BEGIN { print (d > 100) ? 1 : 0 }')"
restored="$(awk -v d="${diff_off:-999}" 'BEGIN { print (d < 50) ? 1 : 0 }')"
turned_on="$(grep -c 'cursor highlight on' "$RUNTIME/comp.log" || true)"
turned_off="$(grep -c 'cursor highlight off' "$RUNTIME/comp.log" || true)"
taps="$(grep -c 'Super tapped' "$RUNTIME/comp.log" || true)"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== cursor highlight (Super+H) ==="
grep -E "cursor highlight (on|off)" "$RUNTIME/comp.log" || true
check "Super+H turns the highlight on" "[[ '$turned_on' -eq 1 ]]"
check "the highlight is actually drawn ($diff_on pixels changed)" "[[ '$drawn' -eq 1 ]]"
check "Super+H turns the highlight off" "[[ '$turned_off' -eq 1 ]]"
check "the output returns to the baseline ($diff_off pixels differ)" "[[ '$restored' -eq 1 ]]"
check "Super+H is not taken as a Super tap" "[[ '$taps' -eq 0 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -25 "$RUNTIME/comp.log"
    echo "==> cursor-highlight test FAILED"
    exit 1
fi
echo "==> cursor-highlight test PASSED"
