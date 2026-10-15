#!/usr/bin/env bash
# Start a nested SDE session (development): run the SDE compositor as a Wayland
# client of the host session, hosting the unmodified SerenityOS shell + apps,
# with the session services and the R4.2 supervisor.
#
# Key detail: the compositor must connect to the *host* display to nest, but the
# apps and the session services it spawns must use the *session* display. The
# compositor sets WAYLAND_DISPLAY for its own children; the separate session
# services (LaunchServer, ImageDecoder) are started here with the session display
# so the apps they spawn join this session, not the host.
#
# Env: HOST_DISPLAY (default wayland-0), SDE_SESSION_DISPLAY (default sde).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
HOST_DISPLAY="${HOST_DISPLAY:-wayland-0}"
SESSION_DISPLAY="${SDE_SESSION_DISPLAY:-sde}"
: "${XDG_RUNTIME_DIR:=/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

rm -f "$XDG_RUNTIME_DIR/$SESSION_DISPLAY"

# Drop the previous session's Xwayland DISPLAY so the session services wait for
# the fresh one (the compositor writes it on startup). Otherwise LaunchServer-
# spawned X11 apps would use a stale/host DISPLAY.
rm -f "${SDE_STATE_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/sde}/xwayland-display"

# Seed the SerenityOS skeleton user config (Browser filters, etc.).
XDG_CONFIG_HOME="${XDG_CONFIG_HOME:-$HOME/.config}" \
  SERENITY="$SERENITY" "$SDE_ROOT/scripts/seed-serenity-home.sh" || true

export SERENITY_RES_ROOT="$SERENITY/Base/res"
export LD_LIBRARY_PATH="$BUILD/lib"
export SDE_APP_BIN_DIR="$BUILD/bin"
export SDE_WM_PORTAL_PATH="/tmp/portal/wm"
export SDE_LOCK_CMD="${SDE_LOCK_CMD:-swaylock}"
export SDE_SESSION_IDLE_SECONDS="${SDE_SESSION_IDLE_SECONDS:-0}"
# Seed a fresh state directory with the intended default wallpaper, as
# start-sde-session.sh does: the compositor resolves a bare name under
# $SERENITY_RES_ROOT/wallpapers. Without this a nested/dev session comes up with
# no wallpaper at all, which hides a missing asset (patches/0066 restores
# blue-grid.png).
export SDE_WALLPAPER="${SDE_WALLPAPER:-blue-grid.png}"
export SDE_WALLPAPER_MODE="${SDE_WALLPAPER_MODE:-fill}"

# Session services; their children must target the session display.
LAUNCH_SERVER="$SDE_ROOT/build/LaunchServer/LaunchServer"
IMAGE_DECODER="$SDE_ROOT/build/ImageDecoder/ImageDecoder"
LAUNCH_PID=""
IMAGE_PID=""
if [ -x "$LAUNCH_SERVER" ]; then
    WAYLAND_DISPLAY="$SESSION_DISPLAY" "$LAUNCH_SERVER" &
    LAUNCH_PID=$!
fi
if [ -x "$IMAGE_DECODER" ]; then
    "$IMAGE_DECODER" &
    IMAGE_PID=$!
fi
# The SDE portal backend (R5): prefer the combined xdg-desktop-portal-serenity
# (implements all the impl.portal.* interfaces), falling back to the R4.4
# Settings-only backend.
PORTAL_PID=""
if [ -x "$SDE_ROOT/build/tools/xdg-desktop-portal-serenity" ]; then
    "$SDE_ROOT/build/tools/xdg-desktop-portal-serenity" &
    PORTAL_PID=$!
elif [ -x "$SDE_ROOT/build/tools/sde-settings-portal" ]; then
    "$SDE_ROOT/build/tools/sde-settings-portal" &
    PORTAL_PID=$!
fi
cleanup() {
    [ -n "$LAUNCH_PID" ] && kill "$LAUNCH_PID" 2>/dev/null || true
    [ -n "$IMAGE_PID" ] && kill "$IMAGE_PID" 2>/dev/null || true
    [ -n "$PORTAL_PID" ] && kill "$PORTAL_PID" 2>/dev/null || true
}
trap cleanup EXIT

STARTUP="$BUILD/bin/Taskbar & $BUILD/bin/FileManager -d"

# The battery status applet (a LibGUI app in this repo) runs in the Taskbar's
# applet area. Build it on demand so a fresh checkout still gets it.
BATTERY_APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"
if [[ ! -x "$BATTERY_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-battery-applet.sh" >/dev/null 2>&1 \
        || echo "note: BatteryApplet not built; skipping the battery applet" >&2
fi
if [[ -x "$BATTERY_APPLET" ]]; then
    STARTUP="$STARTUP & $BATTERY_APPLET"
fi

# The Wi-Fi applet (a LibGUI app that fronts NetworkManager; docs/19).
WIFI_APPLET="$SDE_ROOT/build/WifiApplet/WifiApplet"
if [[ ! -x "$WIFI_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-wifi-applet.sh" >/dev/null 2>&1 \
        || echo "note: WifiApplet not built; skipping the Wi-Fi applet" >&2
fi
if [[ -x "$WIFI_APPLET" ]]; then
    STARTUP="$STARTUP & $WIFI_APPLET"
fi

# The SerenityOS taskbar applets are SDE's; build them on demand so a fresh
# checkout gets them too (scripts/build-applets.sh).
if [[ ! -x "$SDE_ROOT/build/Applets/WorkspacePicker" ]]; then
    "$SDE_ROOT/scripts/build-applets.sh" >/dev/null 2>&1 \
        || echo "note: applets not built; skipping them" >&2
fi
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"
APPLET_CMDS="$(sde_applet_startup_commands "$SDE_ROOT")"
if [[ -n "$APPLET_CMDS" ]]; then
    STARTUP="$STARTUP & $APPLET_CMDS"
fi

# The compositor nests on the host display; its children get the session display
# (the compositor sets WAYLAND_DISPLAY for them).
export WAYLAND_DISPLAY="$HOST_DISPLAY"
export SDE_SESSION_COMPOSITOR="$COMP -n $SESSION_DISPLAY -s \"$STARTUP\""
export SDE_SESSION_DISPLAY="$SESSION_DISPLAY"

"$SDE_ROOT/scripts/sde-session.sh"
status=$?
cleanup
exit "$status"
