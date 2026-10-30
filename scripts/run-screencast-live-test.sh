#!/usr/bin/env bash
# Live ScreenCast infrastructure test (R5).
#
# Unlike run-screencast-test.sh (which forces a headless, pixman-backed
# compositor and can therefore only ever see SHM buffers), this runs against a
# *real* session with an output and a GPU renderer — the thing that can only be
# checked on a TTY/DRM session. It asserts that the compositor can hand the
# ScreenCast portal:
#
#   * DMA-BUF capture formats (zero-copy), and that a frame can actually be
#     captured into an imported DMA-BUF;
#   * a pointer-cursor session with enter/position/hotspot and a real cursor
#     bitmap frame (the metadata cursor mode).
#
# Usage:
#   scripts/run-screencast-live-test.sh            # uses $WAYLAND_DISPLAY
#   WAYLAND_DISPLAY=sde scripts/run-screencast-live-test.sh
#
# With no WAYLAND_DISPLAY (e.g. in CI) it prints a note and passes, so the
# headless suite stays green.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
PROBE="$SDE_ROOT/build/tools/capture-probe"

if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "note: WAYLAND_DISPLAY is unset; skipping the live ScreenCast test"
    echo "==> screencast live test SKIPPED"
    exit 0
fi
# A desktop shell's WAYLAND_DISPLAY is usually the host compositor (KWin, Mutter,
# ...), which has none of SDE's capture protocols, so the test could only fail
# there. Run it against an SDE session only: SDE's compositor is the one that
# advertises serenity_window_manager.
if command -v wayland-info >/dev/null 2>&1 \
    && ! wayland-info 2>/dev/null | grep -q "'serenity_window_manager'"; then
    echo "note: WAYLAND_DISPLAY=$WAYLAND_DISPLAY is not an SDE session; skipping (point it at one, e.g. WAYLAND_DISPLAY=sde)"
    echo "==> screencast live test SKIPPED"
    exit 0
fi

"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

echo "=== compositor capture capabilities (WAYLAND_DISPLAY=$WAYLAND_DISPLAY) ==="
out="$("$PROBE" 2>&1 || true)"
echo "$out"

fail=0
check() {
    if grep -qF "$1" <<<"$out"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

check "output.dmabuf_format"
check "out_frame_dmabuf=1"
check "out_frame_ready=1"
check "cursor_session=1"
check "cursor_frame_ready=1"

if [[ "$fail" -ne 0 ]]; then
    echo "==> screencast live test FAILED"
    exit 1
fi
echo "==> screencast live test PASSED"
