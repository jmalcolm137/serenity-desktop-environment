#!/usr/bin/env bash
# Reproduce the SerenityOS desktop "scene" on SDE: the Taskbar with the applet
# area, a Terminal, a File Manager, a browser (Firefox, standing in for
# Ladybird) and the System Monitor (Performance tab), on the SerenityOS
# blue-grid wallpaper. Handy for a screenshot comparable to
# Meta/Screenshots/screenshot-c03b788.png.
#
# Usage:
#   run-desktop-scene.sh [--headless] [--nested] [--screenshot out.png]
#                        [--wallpaper file.png] [--size WxH]
#
#   --nested (default)  run as a Wayland client of the current host session, so
#                       the scene is visible on screen.
#   --headless          run on the headless backend at --size (default
#                       1024x768); with --screenshot, capture the output to PNG
#                       (via tools/sde-screenshot + wlr-screencopy).
#
# Env: HOST_DISPLAY (default wayland-0), SDE_SESSION_DISPLAY (default sde),
#      SDE_SCENE_FIREFOX (default: the firefox binary), SDE_SCENE_NO_FIREFOX=1
#      to omit the browser.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

MODE="nested"
SCREENSHOT=""
WALLPAPER="$SERENITY/Base/res/wallpapers/blue-grid.png"
SIZE="1280x720"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headless) MODE="headless" ;;
        --nested) MODE="nested" ;;
        --screenshot) SCREENSHOT="$2"; shift ;;
        --wallpaper) WALLPAPER="$2"; shift ;;
        --size) SIZE="$2"; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

# A dedicated session state dir: the wallpaper and (headless) the output layout.
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-scene.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

cleanup() {
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

# --- Wallpaper: convert a PNG to the compositor's raw "SDWP" format ----------
# Header: magic "SDWP", format (Gfx::BitmapFormat::BGRA8888 == 2), width, height,
# pitch, then BGRA pixels (see WindowServer/src/main.cpp).
write_wallpaper() {
    local png="$1"
    [[ -f "$png" ]] || { echo "note: wallpaper '$png' not found; using the theme background" >&2; return; }
    command -v magick >/dev/null || { echo "note: ImageMagick 'magick' not found; no wallpaper" >&2; return; }
    local dims
    dims="$(magick identify -format '%w %h' "$png")"
    local w="${dims%% *}" h="${dims##* }"
    magick "$png" -depth 8 "rgba:$RUNTIME/wallpaper.bgra"
    python3 - "$RUNTIME/wallpaper.bgra" "$RUNTIME/wallpaper.raw" "$w" "$h" <<'PY'
import struct, sys
src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
pixels = open(src, "rb").read()
pitch = w * 4
with open(dst, "wb") as f:
    f.write(struct.pack("<IIIII", 0x53445750, 2, w, h, pitch))
    f.write(pixels[: pitch * h])
PY
    printf 'center' > "$RUNTIME/wallpaper.mode"
}
write_wallpaper "$WALLPAPER"

# --- Seed the skeleton user config so apps find their defaults ---------------
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_CONFIG_HOME="$CONFIG_HOME" SERENITY="$SERENITY" "$SDE_ROOT/scripts/seed-serenity-home.sh" || true

export SERENITY_RES_ROOT="$SERENITY/Base/res"
export LD_LIBRARY_PATH="$BUILD/lib"
export SDE_APP_BIN_DIR="$BUILD/bin"
export SDE_WM_PORTAL_PATH="/tmp/portal/wm"
export SDE_LOCK_CMD="${SDE_LOCK_CMD:-swaylock}"
export SDE_SESSION_IDLE_SECONDS="${SDE_SESSION_IDLE_SECONDS:-0}"

# --- The scene's startup command --------------------------------------------
STARTUP="$BUILD/bin/Taskbar"
STARTUP="$STARTUP & $BUILD/bin/FileManager"
STARTUP="$STARTUP & $BUILD/bin/SystemMonitor -t graphs"

FIREFOX="${SDE_SCENE_FIREFOX:-}"
if [[ -z "$FIREFOX" ]]; then
    for candidate in firefox firefox-esr; do
        if command -v "$candidate" >/dev/null; then FIREFOX="$(command -v "$candidate")"; break; fi
    done
fi
if [[ -n "$FIREFOX" && "${SDE_SCENE_NO_FIREFOX:-0}" != "1" ]]; then
    # A private profile so the scene never disturbs a running browser.
    mkdir -p "$RUNTIME/firefox"
    STARTUP="$STARTUP & MOZ_ENABLE_WAYLAND=1 $FIREFOX --no-remote --profile $RUNTIME/firefox https://serenityos.org"
else
    echo "note: no browser found; the scene will have no browser window" >&2
fi

# Terminal (cascades near the top-left, like the screenshot).
STARTUP="$STARTUP & $BUILD/bin/Terminal"

# Battery applet (SDE's) + the SerenityOS taskbar applets.
BATTERY_APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"
if [[ ! -x "$BATTERY_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-battery-applet.sh" >/dev/null 2>&1 || true
fi
[[ -x "$BATTERY_APPLET" ]] && STARTUP="$STARTUP & $BATTERY_APPLET"

if [[ ! -x "$SDE_ROOT/build/Applets/WorkspacePicker" ]]; then
    "$SDE_ROOT/scripts/build-applets.sh" >/dev/null 2>&1 || true
fi
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"
APPLET_CMDS="$(sde_applet_startup_commands "$SDE_ROOT")"
[[ -n "$APPLET_CMDS" ]] && STARTUP="$STARTUP & $APPLET_CMDS"

# --- Run ---------------------------------------------------------------------
if [[ "$MODE" == "nested" ]]; then
    HOST_DISPLAY="${HOST_DISPLAY:-wayland-0}"
    SESSION_DISPLAY="${SDE_SESSION_DISPLAY:-sde}"
    rm -f "$XDG_RUNTIME_DIR/$SESSION_DISPLAY"

    # Session services (their children must use the session display).
    LAUNCH_PID=""; IMAGE_PID=""; PORTAL_PID=""
    [[ -x "$SDE_ROOT/build/LaunchServer/LaunchServer" ]] && { WAYLAND_DISPLAY="$SESSION_DISPLAY" "$SDE_ROOT/build/LaunchServer/LaunchServer" & LAUNCH_PID=$!; }
    [[ -x "$SDE_ROOT/build/ImageDecoder/ImageDecoder" ]] && { "$SDE_ROOT/build/ImageDecoder/ImageDecoder" & IMAGE_PID=$!; }
    [[ -x "$SDE_ROOT/build/tools/xdg-desktop-portal-serenity" ]] && { "$SDE_ROOT/build/tools/xdg-desktop-portal-serenity" & PORTAL_PID=$!; }
    cleanup_services() {
        for pid in "$LAUNCH_PID" "$IMAGE_PID" "$PORTAL_PID"; do [[ -n "$pid" ]] && kill "$pid" 2>/dev/null || true; done
    }
    trap 'cleanup_services; cleanup' EXIT

    echo "==> SDE desktop scene (nested on $HOST_DISPLAY); close the window to stop"
    export WAYLAND_DISPLAY="$HOST_DISPLAY"
    export SDE_SESSION_COMPOSITOR="$COMP -n $SESSION_DISPLAY -s \"$STARTUP\""
    export SDE_SESSION_DISPLAY="$SESSION_DISPLAY"
    "$SDE_ROOT/scripts/sde-session.sh"
else
    # Headless: fixed output size via screenlayout.ini, then optionally capture.
    SOCK="sde-scene-$$"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"
    cat > "$RUNTIME/screenlayout.ini" <<EOF
[Screens]
MainScreen=0
[Screen0]
Mode=Device
Device=HEADLESS-1
Left=0
Top=0
Width=$WIDTH
Height=$HEIGHT
ScaleFactor=1
EOF

    echo "==> SDE desktop scene (headless ${WIDTH}x${HEIGHT})"
    # The compositor spawns the scene; its children inherit the session display.
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless \
        WLR_HEADLESS_OUTPUTS=1 \
        SDE_STATE_DIR="$RUNTIME" \
        SERENITY_RES_ROOT="$SERENITY_RES_ROOT" \
        LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
        SDE_APP_BIN_DIR="$SDE_APP_BIN_DIR" \
        "$COMP" -n "$SOCK" -s "$STARTUP" > "$RUNTIME/comp.log" 2>&1 &
    COMP_PID=$!

    for _ in $(seq 1 80); do
        [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
        sleep 0.1
    done

    if [[ -n "$SCREENSHOT" ]]; then
        echo "==> letting the scene settle (browser startup can be slow)..."
        sleep "${SDE_SCENE_SETTLE:-12}"
        if [[ ! -x "$SDE_ROOT/build/tools/sde-screenshot" ]]; then
            "$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true
        fi
        PPM="$RUNTIME/scene.ppm"
        WAYLAND_DISPLAY="$SOCK" "$SDE_ROOT/build/tools/sde-screenshot" "$PPM" || true
        kill "$COMP_PID" 2>/dev/null || true
        wait "$COMP_PID" 2>/dev/null || true
        if [[ -s "$PPM" ]]; then
            if command -v magick >/dev/null; then magick "$PPM" "$SCREENSHOT"; elif command -v pnmtopng >/dev/null; then pnmtopng "$PPM" > "$SCREENSHOT"; fi
            echo "==> wrote $SCREENSHOT"
        else
            echo "error: capture failed (see $RUNTIME/comp.log)" >&2
            exit 1
        fi
    else
        echo "==> scene running headless; Ctrl-C to stop"
        wait "$COMP_PID" 2>/dev/null || true
    fi
fi
