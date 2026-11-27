#!/usr/bin/env bash
# ci-tier: full
# LibWM screen capture against the real SDE compositor (serenity-window).
#
# The Magnifier, `shot` and the colour picker ask the WindowServer for pixels.
# Against this compositor the shim takes the direct serenity-window path, not the
# screenshot portal, so this exercises that path end to end:
#   * a whole-screen capture of a window that is *not* repainting (the compositor
#     must schedule a frame for the request, or the caller times out);
#   * a sub-region capture, cropped to the requested size;
#   * the Magnifier's asynchronous around-cursor capture;
#   * the colour under the pointer, sampled from a one-pixel capture.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init libgui-capture
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
sde_test_build WindowServer sde-tools
ninja -C "$BUILD" libwm-test-window >/dev/null

sde_test_compositor LIBWM_WM_PORTAL_PATH="$RUNTIME/wm"

env WAYLAND_DISPLAY="$SOCK" LIBWM_TRACE=1 \
    "$BUILD/bin/libwm-test-window" > "$RUNTIME/capture.log" 2>&1 &
CAPTURE_PID=$!
SDE_TEST_CLIENT_PIDS+=("$CAPTURE_PID")

sde_test_wait_log "$RUNTIME/capture.log" 'sync requests answered' 120 || true
sleep 0.2
sde_test_kill "$CAPTURE_PID"

# The output size is whatever the compositor picked; the capture is in output
# pixels, so it must match.
read -r OUT_W OUT_H <<< "$(sed -n "s/.*output 'HEADLESS-1' \\([0-9]*\\)x\\([0-9]*\\) @.*/\\1 \\2/p" "$RUNTIME/comp.log" | head -1)"
if [[ -z "${OUT_W:-}" ]]; then
    echo "==> could not read the headless output size" >&2
    exit 1
fi

sde_test_check "$RUNTIME/capture.log" 'sync requests answered .*screen_bitmap=true' "the whole screen was captured"
sde_test_check "$RUNTIME/capture.log" "screenshot whole ${OUT_W}x${OUT_H}" "the capture matches the output size"
sde_test_check "$RUNTIME/capture.log" 'screenshot region 200x200' "a requested region is cropped to its size"
sde_test_check "$RUNTIME/capture.log" 'sync requests answered .*around_cursor=true' "the Magnifier's capture returned an image"
sde_test_check "$RUNTIME/capture.log" 'sync requests answered .*around_location=true' "the Magnifier's locked-location capture returned an image"
sde_test_check "$RUNTIME/capture.log" 'color under cursor #[0-9a-f]{6}' "the colour under the pointer was sampled"
sde_test_check "$RUNTIME/comp.log" 'capture served' "the compositor served the capture"

sde_test_stop_compositor
sde_test_no_fault "$RUNTIME/comp.log" "$RUNTIME/capture.log"
sde_test_finish "$RUNTIME/capture.log"
