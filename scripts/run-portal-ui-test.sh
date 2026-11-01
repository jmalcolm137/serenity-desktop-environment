#!/usr/bin/env bash
# UI smoke test for the portal's LibGUI dialogs (R5): it starts a headless SDE
# compositor, launches each dialog with sample data, screenshots it, and asserts
# that the client area actually rendered (more than a couple of colours). This
# catches the "blank dialog" failure mode (e.g. a missing background fill).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init portal-ui
sde_test_export_roots
SHOT="$SDE_ROOT/build/tools/sde-screenshot"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
"$SDE_ROOT/scripts/build-portal-apps.sh" >/dev/null

if ! command -v convert >/dev/null 2>&1; then
    echo "==> portal UI test SKIPPED (ImageMagick convert not found)"
    exit 0
fi

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"
sleep 0.4

convert -size 64x64 gradient:red-blue "$RUNTIME/gradient.png" 2>/dev/null || true

fail_dialog() {
    echo "  FAIL: $1"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
}

check_dialog() {
    local name="$1"; shift
    local bin="$SDE_ROOT/build/$name/$name"
    if [[ ! -x "$bin" ]]; then
        echo "  note: $name not built; skipping"
        return
    fi
    sde_test_client env WAYLAND_DISPLAY="$SOCK" "$bin" "$@" > "$RUNTIME/$name.log" 2>&1
    local pid=$SDE_TEST_CLIENT_PID
    sleep 2.5
    if ! kill -0 "$pid" 2>/dev/null; then
        fail_dialog "$name exited early ($(head -1 "$RUNTIME/$name.log"))"
        return
    fi
    local ppm="$RUNTIME/$name.ppm"
    if ! WAYLAND_DISPLAY="$SOCK" "$SHOT" "$ppm" >/dev/null 2>&1; then
        fail_dialog "$name screenshot failed"
        sde_test_kill "$pid"
        return
    fi
    local colors
    colors="$(convert "$ppm" -crop 360x200+100+100 +repage -format "%k" info: 2>/dev/null || echo 0)"
    if [[ "${colors:-0}" -lt 4 ]]; then
        # A dialog that positions itself explicitly may still be at the origin.
        local colors_origin
        colors_origin="$(convert "$ppm" -crop 360x200+0+0 +repage -format "%k" info: 2>/dev/null || echo 0)"
        if [[ "${colors_origin:-0}" -gt "${colors:-0}" ]]; then
            colors="$colors_origin"
        fi
    fi
    if [[ "${colors:-0}" -ge 4 ]]; then
        echo "  ok: $name rendered ($colors colours)"
    else
        fail_dialog "$name looks blank ($colors colours)"
    fi
    sde_test_kill "$pid"
    sleep 0.3
}

echo "=== portal dialogs render ==="
check_dialog AccessDialog -t "Allow microphone access?" -s "App: Recorder" -b "This application wants to use the microphone."
check_dialog PrintDialog -t "Print" -p "HP LaserJet, Print to File"
check_dialog AppChooserDialog -t "Open with" -c "org.serenity.Browser
org.serenity.TextEditor"
check_dialog ColorPicker -i "$RUNTIME/gradient.png"
check_dialog ScreenshotOverlay -i "$RUNTIME/gradient.png"
check_dialog ScreenCastChooser -t "Share your screen" "Entire screen" "Window: TextEditor"
check_dialog FileChooserDialog -m open -t "Open File" -f "$SERENITY/Base/res"

sde_test_finish
