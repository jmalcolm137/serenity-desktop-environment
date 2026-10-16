#!/usr/bin/env bash
# Headless window-stack-switch overlay test (R3).
#
# SerenityOS briefly shows a grid of one cell per window stack (workspace), with
# the current one highlighted, when the stack is switched (and when the user asks
# for the stack they are already on). The compositor draws it as a ChromeNode
# overlay for 500 ms.
#
# This drives a headless compositor: capture the output, switch workspace
# (Ctrl+Alt+Right), capture (the overlay must be visible), wait for the timeout,
# capture again (it must be gone), asserting the log and the pixels.
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
    echo "==> stack-overlay test SKIPPED (ImageMagick compare not found)"
    exit 0
fi

WIDTH="${SDE_STACK_WIDTH:-800}"
HEIGHT="${SDE_STACK_HEIGHT:-600}"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-stack.XXXXXX")"
chmod 700 "$RUNTIME"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

SOCK="sde-stack-$$"
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 2

capture() { env WAYLAND_DISPLAY="$SOCK" "$SHOT" "$1" >/dev/null 2>&1; }

capture "$RUNTIME/before.ppm"

# Ctrl+Alt+Right: switch to the next stack. The input tool flushes as it goes, so
# run it in the background and capture as soon as the compositor logs the overlay
# (it is only up for 500 ms).
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --key 29 press --sleep 60 --key 56 press --sleep 60 \
    --key 106 press --sleep 60 --key 106 release --sleep 60 \
    --key 56 release --sleep 60 --key 29 release --linger 300 >/dev/null 2>&1 &
VI_PID=$!
for _ in $(seq 1 100); do
    grep -q 'window-stack switch overlay for workspace 0,1' "$RUNTIME/comp.log" && break
    sleep 0.02
done
capture "$RUNTIME/during.ppm"
wait "$VI_PID" 2>/dev/null || true

# The overlay is removed after 500 ms.
sleep 1
capture "$RUNTIME/after.ppm"

diff_on="$(compare -metric AE "$RUNTIME/before.ppm" "$RUNTIME/during.ppm" null: 2>&1 | awk '{print $1}' || true)"
diff_off="$(compare -metric AE "$RUNTIME/before.ppm" "$RUNTIME/after.ppm" null: 2>&1 | awk '{print $1}' || true)"
drawn="$(awk -v d="${diff_on:-0}" 'BEGIN { print (d > 100) ? 1 : 0 }')"
gone="$(awk -v d="${diff_off:-999}" 'BEGIN { print (d < 50) ? 1 : 0 }')"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== window-stack switch overlay ==="
grep "window-stack switch overlay" "$RUNTIME/comp.log" || true
check "a workspace switch shows the overlay" "[[ \"\$(grep -c 'window-stack switch overlay for workspace 0,1' \"\$RUNTIME/comp.log\")\" -ge 1 ]]"
check "the overlay is drawn ($diff_on pixels changed)" "[[ '$drawn' -eq 1 ]]"
check "the overlay goes away again ($diff_off pixels differ)" "[[ '$gone' -eq 1 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -15 "$RUNTIME/comp.log"
    echo "==> stack-overlay test FAILED"
    exit 1
fi
echo "==> stack-overlay test PASSED"
