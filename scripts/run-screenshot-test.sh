#!/usr/bin/env bash
# Screenshot-tool test: `sde-screenshot` captures the (headless) output and exits
# cleanly. Guards the crash where the compositor handed back a 24-bit shm format
# (BGR888/BG24, 3 bytes/pixel) but the tool assumed 32-bit, read past the end of
# the mapping and segfaulted mid-capture (leaving a truncated PPM).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init screenshot
sde_test_export_roots
SHOT="$SDE_ROOT/build/tools/sde-screenshot"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

sde_test_compositor
sleep 0.5

echo "=== sde-screenshot captures and exits cleanly ==="
set +e
WAYLAND_DISPLAY="$SOCK" SDE_SCREENSHOT_DEBUG=1 "$SHOT" "$RUNTIME/shot.ppm" > "$RUNTIME/shot.log" 2>&1
rc=$?
set -e
sde_test_expect "exit 0 (exit $rc)" test "$rc" -eq 0
if grep -q "format=" "$RUNTIME/shot.log"; then echo "  ok: advertised $(grep -o 'format=0x[0-9a-f]* ([^)]*) [0-9]*x[0-9]* stride=[0-9]* bpp=[0-9]*' "$RUNTIME/shot.log" | head -1)"; fi

# The PPM must be complete: a P6 header plus width*height*3 bytes (a partial write
# from a crash leaves a short file).
magic="" w=0 h=0 max=""
{ read -r magic; read -r w h; read -r max; } < "$RUNTIME/shot.ppm" || true
sde_test_expect "P6 header ${w}x${h}" test "$magic" = P6 -a "$w" -gt 0 -a "$h" -gt 0 -a "$max" = 255
expected_body=$(( w * h * 3 ))
actual=$(stat -c %s "$RUNTIME/shot.ppm" 2>/dev/null || echo 0)
header_bytes=$(( actual - expected_body ))
sde_test_expect "full image ($actual bytes, ${header_bytes}-byte header; body $expected_body)" \
    test "$header_bytes" -ge 11 -a "$header_bytes" -le 32
sde_test_refute "$RUNTIME/shot.log" "SIGSEGV|Segmentation fault" "the tool did not crash"
sde_test_finish "$RUNTIME/shot.log"
