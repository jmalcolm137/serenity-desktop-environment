#!/usr/bin/env bash
# UI smoke test for the portal's LibGUI dialogs (R5): it starts a headless SDE
# compositor, launches each dialog with sample data, screenshots it, and asserts
# that the client area actually rendered (more than a couple of colours). This
# catches the "blank dialog" failure mode (e.g. a missing background fill).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SHOT="$SDE_ROOT/build/tools/sde-screenshot"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
"$SDE_ROOT/scripts/build-portal-apps.sh" >/dev/null

if ! command -v convert >/dev/null 2>&1; then
    echo "==> portal UI test SKIPPED (ImageMagick convert not found)"
    exit 0
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-portal-ui.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

SOCK="sde-pui-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
trap 'kill "$COMP_PID" 2>/dev/null || true; for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.4

convert -size 64x64 gradient:red-blue "$RUNTIME/gradient.png" 2>/dev/null || true

fail=0
check_dialog() {
    local name="$1"; shift
    local bin="$SDE_ROOT/build/$name/$name"
    if [[ ! -x "$bin" ]]; then
        echo "  note: $name not built; skipping"
        return
    fi
    env WAYLAND_DISPLAY="$SOCK" SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" "$bin" "$@" > "$RUNTIME/$name.log" 2>&1 &
    local pid=$!
    sleep 2.5
    if ! kill -0 "$pid" 2>/dev/null; then
        echo "  FAIL: $name exited early ($(head -1 "$RUNTIME/$name.log"))"
        fail=1
        return
    fi
    local ppm="$RUNTIME/$name.ppm"
    if ! WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" "$SHOT" "$ppm" >/dev/null 2>&1; then
        echo "  FAIL: $name screenshot failed"
        fail=1
        kill "$pid" 2>/dev/null || true
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
        echo "  FAIL: $name looks blank ($colors colours)"
        fail=1
    fi
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
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

if [[ "$fail" -ne 0 ]]; then
    echo "==> portal UI test FAILED"
    exit 1
fi
echo "==> portal UI test PASSED"
