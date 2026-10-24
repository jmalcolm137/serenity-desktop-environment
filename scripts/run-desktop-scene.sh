#!/usr/bin/env bash
# Reproduce the SerenityOS desktop "scene" on SDE: the Taskbar with the applet
# area, a Terminal, a File Manager, a Browser (Ladybird) and the System
# Monitor (Performance tab), on the SerenityOS
# blue-grid wallpaper. Handy for a screenshot comparable to
# Meta/Screenshots/screenshot-c03b788.png.
#
# Usage:
#   run-desktop-scene.sh [--headless] [--nested] [--screenshot out.png]
#                        [--wallpaper file.png] [--size WxH] [--scale N]
#
#   --nested (default)  run as a Wayland client of the current host session, so
#                       the scene is visible on screen.
#   --headless          run on the headless backend at --size (default
#                       1024x768); with --screenshot, capture the output to PNG
#                       (via Tools/sde-screenshot + wlr-screencopy).
#
# Env: HOST_DISPLAY (default wayland-0), SDE_SESSION_DISPLAY (default sde),
#      SDE_SCENE_BROWSER (default: the built-in Browser), SDE_SCENE_NO_BROWSER=1
#      to omit the browser.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

MODE="nested"
SCREENSHOT=""
# The SerenityOS grid wallpaper (the reference screenshot's "blue grid"). Older
# SerenityOS shipped it as blue-grid.png; the current tree names it grid.png.
WALLPAPER=""
for candidate in blue-grid.png grid.png; do
    if [[ -f "$SERENITY/Base/res/wallpapers/$candidate" ]]; then
        WALLPAPER="$SERENITY/Base/res/wallpapers/$candidate"
        break
    fi
done
SIZE="1280x720"
SCALE="1"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --headless) MODE="headless" ;;
        --nested) MODE="nested" ;;
        --screenshot) SCREENSHOT="$2"; shift ;;
        --wallpaper) WALLPAPER="$2"; shift ;;
        --size) SIZE="$2"; shift ;;
        --scale) SCALE="$2"; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done

# SIZE is the logical size (used for window placement and virtual-input extent);
# --scale makes the headless output that many times larger physically (a HiDPI
# capture: e.g. --size 1280x720 --scale 2 -> a 2560x1440 screenshot at 2x).
WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

# Keep the Taskbar current: it drives the applet-area position over the
# WindowManagerServer connection, so a Taskbar binary that is out of sync with
# LibGUI leaves the applets at the usable-area fallback (above the taskbar).
"$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null 2>&1 || true

# A dedicated session state dir: the wallpaper and (headless) the output layout.
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-scene.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
# Keep scene services off the live session's /tmp/session/0 portal sockets.
# In particular, Browser creates a WebView database through SQLServer; sharing
# the live SQL socket can make it fail before its first window is mapped.
export SDE_SESSION_DIR="$RUNTIME/session"
mkdir -p "$SDE_SESSION_DIR/portal"
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

cleanup() {
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

# --- Wallpaper: convert a PNG to the compositor's raw "SDWP" format ----------
# Header: magic "SDWP", format (Gfx::BitmapFormat::BGRA8888 == 2), width, height,
# pitch, then BGRA pixels (see Services/WindowServer/src/Wallpaper.cpp).
write_wallpaper() {
    local png="$1"
    [[ -f "$png" ]] || { echo "note: wallpaper '$png' not found; using the theme background" >&2; return; }
    command -v magick >/dev/null || { echo "note: ImageMagick 'magick' not found; no wallpaper" >&2; return; }
    local dims
    dims="$(magick identify -format '%w %h' "$png")"
    local w="${dims%% *}" h="${dims##* }"
    magick "$png" -depth 8 "bgra:$RUNTIME/wallpaper.bgra"
    python3 - "$RUNTIME/wallpaper.bgra" "$RUNTIME/wallpaper.raw" "$w" "$h" <<'PY'
import struct, sys
src, dst, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
pixels = open(src, "rb").read()
pitch = w * 4
with open(dst, "wb") as f:
    f.write(struct.pack("<IIIII", 0x53445750, 2, w, h, pitch))
    f.write(pixels[: pitch * h])
PY
    printf 'fill' > "$RUNTIME/wallpaper.mode"
}

# --- Window placement rules ([Placement] in WindowServer.ini) ---------------
# Roughly the reference layout (Terminal top-left, File Manager top-right,
# browser bottom-left, System Monitor bottom-right) scaled to WIDTHxHEIGHT.
write_placement() {
    local right=$((WIDTH / 2))
    local top=$((HEIGHT * 45 / 100))
    local bottom=$((HEIGHT * 47 / 100))
    cat > "$RUNTIME/WindowServer.ini" <<EOF
[Placement]
Windows=Terminal:0,0,${right},${top};File Manager:${right},0,$((WIDTH - right)),${top};Browser:0,${top},${right},${bottom};System Monitor:${right},${top},$((WIDTH - right)),${bottom}
[Applet]
Order=WorkspacePicker,CPUGraph,MemoryGraph,NetworkGraph,Network,ClipboardHistory,Audio,Keymap,WiFi,Battery
EOF
}
write_placement
write_wallpaper "$WALLPAPER"

# --- Seed the skeleton user config so apps find their defaults ---------------
CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}"
XDG_CONFIG_HOME="$CONFIG_HOME" SERENITY="$SERENITY" "$SDE_ROOT/scripts/seed-serenity-home.sh" || true

export SERENITY_RES_ROOT="$SERENITY/Base/res"
export LD_LIBRARY_PATH="$BUILD/lib"
export SDE_APP_BIN_DIR="$BUILD/bin"
export SDE_WM_PORTAL_PATH="/tmp/portal/wm"
SDE_LOCK_BIN="${SDE_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}/build/tools/sde-lock"
if [ -z "${SDE_LOCK_CMD:-}" ]; then
    if [ -x "$SDE_LOCK_BIN" ]; then SDE_LOCK_CMD="$SDE_LOCK_BIN"; else SDE_LOCK_CMD=swaylock; fi
fi
export SDE_LOCK_CMD
export SDE_SESSION_IDLE_SECONDS="${SDE_SESSION_IDLE_SECONDS:-0}"

# --- The scene's startup command --------------------------------------------
STARTUP="$BUILD/bin/Taskbar"
STARTUP="$STARTUP & $BUILD/bin/FileManager"
STARTUP="$STARTUP & $BUILD/bin/SystemMonitor -t graphs"

# Browser (SerenityOS's, i.e. Ladybird) - the reference app. SDE_SCENE_BROWSER
# overrides the binary; SDE_SCENE_NO_BROWSER=1 omits it.
BROWSER="${SDE_SCENE_BROWSER:-$BUILD/bin/Browser}"
if [[ "${SDE_SCENE_NO_BROWSER:-0}" != "1" && -x "$BROWSER" ]]; then
    # A stale Ladybird single-instance socket/pid makes Browser defer to a
    # session service and never map a window.
    rm -f "${XDG_RUNTIME_DIR}/Ladybird.socket" "${XDG_RUNTIME_DIR}/Ladybird.pid"
    STARTUP="$STARTUP & $BROWSER https://serenityos.org"
else
    echo "note: no browser; the scene will have no browser window" >&2
fi

# Terminal (top-left, like the screenshot). It runs an interactive shell with a
# clean prompt; the screenshot step then *types* `uname -a` and `df -Th` into it
# (via virtual input), so the capture shows the prompt, the commands and their
# output — like a user session — not just the output.
SCENE_PS1="$(whoami)@$(hostname -s):~$ "
{
    printf 'export PS1=%q\n' "$SCENE_PS1"
    printf 'cd ~\n'
    printf 'exec bash --norc -i\n'
} > "$RUNTIME/scene-shell"
chmod +x "$RUNTIME/scene-shell"
STARTUP="$STARTUP & $BUILD/bin/Terminal -e $RUNTIME/scene-shell"

# Battery applet (SDE's) + the SerenityOS taskbar applets.
BATTERY_APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"
if [[ ! -x "$BATTERY_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-battery-applet.sh" >/dev/null 2>&1 || true
fi
[[ -x "$BATTERY_APPLET" ]] && STARTUP="$STARTUP & $BATTERY_APPLET"

# Wi-Fi applet (SDE's; fronts NetworkManager — docs/19).
WIFI_APPLET="$SDE_ROOT/build/WifiApplet/WifiApplet"
if [[ ! -x "$WIFI_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-wifi-applet.sh" >/dev/null 2>&1 || true
fi
[[ -x "$WIFI_APPLET" ]] && STARTUP="$STARTUP & $WIFI_APPLET"

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
    # The mode is the *physical* size (logical * scale); SDE_OUTPUT_SCALE carries
    # the scale so the compositor maps it back to the logical size.
    SOCK="sde-scene-$$"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"
    PHYS_W="$(awk "BEGIN { printf \"%d\", $WIDTH * $SCALE }")"
    PHYS_H="$(awk "BEGIN { printf \"%d\", $HEIGHT * $SCALE }")"
    cat > "$RUNTIME/screenlayout.ini" <<EOF
[Screens]
MainScreen=0
[Screen0]
Mode=Device
Device=HEADLESS-1
Left=0
Top=0
Width=$PHYS_W
Height=$PHYS_H
ScaleFactor=1
EOF

    echo "==> SDE desktop scene (headless ${WIDTH}x${HEIGHT} @${SCALE}x)"
    # The compositor spawns the scene; its children inherit the session display.
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless \
        WLR_HEADLESS_OUTPUTS=1 \
        SDE_OUTPUT_SCALE="$SCALE" \
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
        sleep "${SDE_SCENE_SETTLE:-20}"
        # Type `uname -a` and `df -Th` into the Terminal (so the capture shows the
        # prompt, the commands and the output), then click the File Manager (the
        # reference screenshot has File Manager as the active window — exactly one
        # window is active, and its Taskbar button is raised), then open the
        # Taskbar's system menu
        # and the Development submenu, like the reference screenshot. (The menu
        # needs a mouse-down on the Taskbar, so click the start button rather than
        # using the Super key.)
        VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

        # Minimal char -> evdev keycode map (enough for the two commands).
        declare -A EVDEV=(
            [a]=30 [b]=48 [c]=46 [d]=32 [e]=18 [f]=33 [g]=34 [h]=35 [i]=23
            [j]=36 [k]=37 [l]=38 [m]=50 [n]=49 [o]=24 [p]=25 [q]=16 [r]=19
            [s]=31 [t]=20 [u]=22 [v]=47 [w]=17 [x]=45 [y]=21 [z]=44
            [0]=11 [1]=2 [2]=3 [3]=4 [4]=5 [5]=6 [6]=7 [7]=8 [8]=9 [9]=10
            [" "]=57 ["-"]=12 ["."]=52 ["/"]=53 [":"]=39
        )
        KEY_ARGS=()
        add_key() { KEY_ARGS+=(--key "$1" press --key "$1" release --sleep 25); }
        type_string() {
            local s="$1" i ch code
            for (( i = 0; i < ${#s}; ++i )); do
                ch="${s:i:1}"
                if [[ "$ch" =~ [A-Z] ]]; then
                    ch="${ch,,}"
                    KEY_ARGS+=(--key 42 press)
                    add_key "${EVDEV[$ch]:-}"
                    KEY_ARGS+=(--key 42 release)
                else
                    add_key "${EVDEV[$ch]:-}"
                fi
            done
            add_key 28 # Enter
            sleep 0.1
        }
        type_string "uname -a"
        type_string "df -Th"

        env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
            --move 200 120 --button 0x110 press --sleep 60 --button 0x110 release --sleep 500 \
            "${KEY_ARGS[@]}" \
            --sleep 300 \
            --move $((WIDTH * 3 / 4)) 10 --button 0x110 press --sleep 60 --button 0x110 release --sleep 400 \
            --move 20 $((HEIGHT - 14)) --button 0x110 press --sleep 80 --button 0x110 release --sleep 400 \
            --move 60 $((HEIGHT - 326)) --sleep 800 \
            --move 300 $((HEIGHT - 206)) --sleep 900 >/dev/null 2>&1 || true
        sleep 0.5
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
