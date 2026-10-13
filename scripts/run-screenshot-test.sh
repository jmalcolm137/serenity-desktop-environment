#!/usr/bin/env bash
# Screenshot-tool test: `sde-screenshot` captures the (headless) output and exits
# cleanly. Guards the crash where the compositor handed back a 24-bit shm format
# (BGR888/BG24, 3 bytes/pixel) but the tool assumed 32-bit, read past the end of
# the mapping and segfaulted mid-capture (leaving a truncated PPM).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SHOT="$SDE_ROOT/build/tools/sde-screenshot"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

fail=0

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-shot.XXXXXX")"
chmod 700 "$R"
SOCK="sde-shot-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$R" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
CP=$!
trap 'kill "$CP" 2>/dev/null || true; rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" 2>/dev/null || true' EXIT
for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
sleep 0.5

echo "=== sde-screenshot captures and exits cleanly ==="
set +e
WAYLAND_DISPLAY="$SOCK" SDE_SCREENSHOT_DEBUG=1 "$SHOT" "$R/shot.ppm" > "$R/shot.log" 2>&1
rc=$?
set -e
if [[ "$rc" -eq 0 ]]; then echo "  ok: exit 0"; else echo "  FAIL: exit $rc (crash?)"; fail=1; fi
if grep -q "format=" "$R/shot.log"; then echo "  ok: advertised $(grep -o 'format=0x[0-9a-f]* ([^)]*) [0-9]*x[0-9]* stride=[0-9]* bpp=[0-9]*' "$R/shot.log" | head -1)"; fi

# The PPM must be complete: a P6 header plus width*height*3 bytes (a partial write
# from a crash leaves a short file).
{ read -r magic; read -r w h; read -r max; } < "$R/shot.ppm"
if [[ "$magic" == "P6" && "$w" -gt 0 && "$h" -gt 0 && "$max" == "255" ]]; then
    echo "  ok: P6 header ${w}x${h}"
else
    echo "  FAIL: bad PPM header ('$magic $w $h $max')"; fail=1
fi
expected_body=$(( w * h * 3 ))
actual=$(stat -c %s "$R/shot.ppm")
header_bytes=$(( actual - expected_body ))
if [[ "$header_bytes" -ge 11 && "$header_bytes" -le 32 ]]; then
    echo "  ok: full image ($actual bytes, ${header_bytes}-byte header)"
else
    echo "  FAIL: truncated/incomplete image ($actual bytes; body should be $expected_body)"; fail=1
fi
if grep -qiE "SIGSEGV|Segmentation fault" "$R/shot.log"; then echo "  FAIL: the tool crashed"; fail=1; fi

if [[ "$fail" -ne 0 ]]; then echo "==> screenshot test FAILED"; exit 1; fi
echo "==> screenshot test PASSED"
