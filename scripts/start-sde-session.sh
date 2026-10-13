#!/usr/bin/env bash
# Start an SDE session on real hardware (DRM/KMS + libinput).
#
# Run this from a TTY (e.g. Ctrl+Alt+F3) as the logged-in user, or from a
# display manager's session entry. With WAYLAND_DISPLAY/DISPLAY unset,
# wlr_backend_autocreate selects the DRM+libinput backend and libseat grants the
# compositor DRM master. The compositor serves the WindowManagerServer on the
# standard /tmp/portal/wm socket and starts the shell.
#
# Usage: start-sde-session.sh ["<startup command>"]
#   The startup command defaults to running the Taskbar and a TextEditor.
#
# Set SDE_BACKENDS to force a backend list (passed to wlr as WLR_BACKENDS), e.g.
# SDE_BACKENDS=headless ./start-sde-session.sh
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

: "${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR must be set (log in to a proper session)}"

if [[ ! -x "$COMP" ]]; then
    echo "error: $COMP not found; run scripts/build-window-server.sh first" >&2
    exit 1
fi

# R4.3/R6 multi-seat: seat identity + per-seat runtime namespaces (see
# scripts/sde-seat.sh). SDE_SESSION_DISPLAY and SDE_WM_PORTAL_PATH are exported so
# the supervisor, the session services and the apps all agree.
# shellcheck source=scripts/sde-seat.sh
. "$SDE_ROOT/scripts/sde-seat.sh"
sde_detect_seat
export SDE_SESSION_DISPLAY="${SDE_SESSION_DISPLAY:-sde$(sde_seat_suffix)}"
SDE_WM_PORTAL_PATH="${SDE_WM_PORTAL_PATH:-$(sde_wm_portal_path)}"
export SDE_WM_PORTAL_PATH
echo "SDE: seat '${XDG_SEAT}'${XDG_VTNR:+ vt$XDG_VTNR} display '$SDE_SESSION_DISPLAY' wm-portal '$SDE_WM_PORTAL_PATH'"

# R4.3/R6: a native (DRM+libinput) session needs a seat and a DRM device. Give
# actionable guidance instead of a loop of opaque backend failures. Override with
# SDE_ALLOW_NO_DRM=1 (and SDE_DRM_GLOB for tests).
if [[ -z "${SDE_BACKENDS:-}" && "${SDE_ALLOW_NO_DRM:-0}" != "1" ]]; then
    drm_glob="${SDE_DRM_GLOB:-/dev/dri/card*}"
    if ! compgen -G "$drm_glob" >/dev/null 2>&1; then
        cat >&2 <<EOF
SDE: no DRM device ($drm_glob) — the native DRM+libinput backend cannot start.
  * for a headless run:  SDE_BACKENDS=headless $0
  * for development:     ./scripts/start-sde-nested.sh
  * on a TTY, make sure the DRM device exists and your user can open it (a seat:
    seatd or logind, or the 'video'/'render'/'input' groups for the builtin seat).
  * set SDE_ALLOW_NO_DRM=1 to skip this check.
EOF
        exit 1
    fi
fi

# Hardware session: no host compositor underneath us.
unset WAYLAND_DISPLAY DISPLAY
export SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}"
export LD_LIBRARY_PATH="$BUILD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# The applications run from this build's bin directory; the session LaunchServer
# (below) is not built next to them, so point it there. See
# docs/06-service-layering.md.
export SDE_APP_BIN_DIR="${SDE_APP_BIN_DIR:-$BUILD/bin}"

# Super+L locks the session by spawning $SDE_LOCK_CMD (default: swaylock), a
# standard ext-session-lock-v1 client. See docs/11-r4-implementation-plan.md §9.
export SDE_LOCK_CMD="${SDE_LOCK_CMD:-swaylock}"

# Idle-to-lock after this many seconds (0 disables). Uses the portable sde-idle
# daemon (a plain ext-idle-notify-v1 client). Build it with scripts/build-tools.sh.
export SDE_SESSION_IDLE_SECONDS="${SDE_SESSION_IDLE_SECONDS:-300}"
# Blank the screen (sde-dpms) this many seconds before locking (0 disables).
export SDE_SESSION_IDLE_DPMS_SECONDS="${SDE_SESSION_IDLE_DPMS_SECONDS:-60}"

# Seed the SerenityOS skeleton user config, so apps that read default
# configuration at startup (e.g. Browser) find it.
SERENITY="$SERENITY" "$SDE_ROOT/scripts/seed-serenity-home.sh"

# Start the Taskbar and a TextEditor, plus the taskbar applets, unless the
# caller supplies their own startup command.
BATTERY_APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"
if [[ ! -x "$BATTERY_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-battery-applet.sh" >/dev/null 2>&1 \
        || echo "note: BatteryApplet not built; skipping the battery applet" >&2
fi

# The SerenityOS taskbar applets (WorkspacePicker, ResourceGraph, Network, ...)
# are SDE's; build them on demand so a fresh checkout gets them too.
if [[ ! -x "$SDE_ROOT/build/Applets/WorkspacePicker" ]]; then
    "$SDE_ROOT/scripts/build-applets.sh" >/dev/null 2>&1 \
        || echo "note: applets not built; skipping them" >&2
fi
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"
APPLET_CMDS="$(sde_applet_startup_commands "$SDE_ROOT")"

# R6/N4: a minimal startup for safe mode (used automatically when the compositor
# keeps failing, and with SDE_SAFE_MODE=1): just the shell, no applets/battery.
SAFE_STARTUP="$BUILD/bin/Taskbar"

if [[ "${SDE_SAFE_MODE:-0}" == "1" ]]; then
    # Manual safe mode: minimal shell, and tell the supervisor to skip autostart.
    STARTUP="$SAFE_STARTUP"
    export SDE_SESSION_SAFE_MODE=1
elif [[ -n "${1:-}" ]]; then
    STARTUP="$1"
else
    STARTUP="$BUILD/bin/Taskbar & $BUILD/bin/TextEditor"
    if [[ -n "$APPLET_CMDS" ]]; then
        STARTUP="$STARTUP & $APPLET_CMDS"
    fi
    if [[ -x "$BATTERY_APPLET" ]]; then
        STARTUP="$STARTUP & $BATTERY_APPLET"
    fi
fi

if [[ -n "${SDE_BACKENDS:-}" ]]; then
    export WLR_BACKENDS="$SDE_BACKENDS"
fi

# The session LaunchServer owns the launch portal when it is built and running;
# without it, applications fall back to the in-process LaunchServer inside
# libgui-wayland. Either way launching works.
#
# IMPORTANT: a session service that spawns applications must run with the
# session's WAYLAND_DISPLAY, or its children connect to the wrong compositor
# (e.g. the host when nested). The compositor sets it for its own children; the
# services are separate processes, so set it here.
SESSION_DISPLAY="$SDE_SESSION_DISPLAY"
LAUNCH_SERVER="$SDE_ROOT/build/LaunchServer/LaunchServer"
LAUNCH_SERVER_PID=""
if [[ -x "$LAUNCH_SERVER" ]]; then
    WAYLAND_DISPLAY="$SESSION_DISPLAY" "$LAUNCH_SERVER" &
    LAUNCH_SERVER_PID=$!
fi

# The session ImageDecoder owns the image portal when it is built and running;
# without it, applications fall back to the in-process ImageDecoder inside
# libgui-wayland. Either way image decoding works.
IMAGE_DECODER="$SDE_ROOT/build/ImageDecoder/ImageDecoder"
IMAGE_DECODER_PID=""
if [[ -x "$IMAGE_DECODER" ]]; then
    "$IMAGE_DECODER" &
    IMAGE_DECODER_PID=$!
fi

# The SDE portal backend (R5): xdg-desktop-portal-serenity implements the
# org.freedesktop.impl.portal.* interfaces (Settings, ScreenCast, RemoteDesktop,
# FileChooser, Screenshot, Print, ...) that portals.conf selects, so
# xdg-desktop-portal (and thus GTK/Qt/Flatpak apps) follow the SDE session.
# Falls back to the R4.4 Settings-only backend; harmless when xdg-desktop-portal
# is absent.
PORTAL_BACKEND="$SDE_ROOT/build/tools/xdg-desktop-portal-serenity"
SETTINGS_PORTAL="$SDE_ROOT/build/tools/sde-settings-portal"
PORTAL_PID=""
if [[ -x "$PORTAL_BACKEND" ]]; then
    "$PORTAL_BACKEND" &
    PORTAL_PID=$!
elif [[ -x "$SETTINGS_PORTAL" ]]; then
    "$SETTINGS_PORTAL" &
    PORTAL_PID=$!
fi

echo "SDE: starting WindowServer (${SDE_BACKENDS:-drm,libinput}) on $XDG_RUNTIME_DIR"
# Build the portable session tools (sde-idle) if needed.
if [ ! -x "$SDE_ROOT/build/tools/sde-idle" ]; then
    "$SDE_ROOT/scripts/build-tools.sh" >/dev/null
fi
# The session supervisor (R4.2) starts/restarts the compositor, runs XDG
# Autostart, owns the session environment, and arms idle-to-lock.
export SDE_SESSION_COMPOSITOR="$COMP -n $SESSION_DISPLAY -s \"$STARTUP\""
# R6/N4: if the compositor keeps failing, the supervisor drops to safe mode with
# this minimal shell instead of ending the session.
export SDE_SESSION_COMPOSITOR_SAFE="$COMP -n $SESSION_DISPLAY -s \"$SAFE_STARTUP\""

# Run the supervisor in the background so a display manager's logout (SIGTERM to
# the session) stops it cleanly here rather than leaving it orphaned: the trap
# forwards the signal, and we wait for it to actually exit.
"$SDE_ROOT/scripts/sde-session.sh" &
SUPERVISOR_PID=$!
stopping=0
# shellcheck disable=SC2064
trap 'stopping=1; kill -TERM "$SUPERVISOR_PID" 2>/dev/null || true' TERM INT
wait "$SUPERVISOR_PID"
status=$?
if [[ "$stopping" -eq 1 ]]; then
    wait "$SUPERVISOR_PID" 2>/dev/null || true
    status=0
fi
trap - TERM INT

if [[ -n "$LAUNCH_SERVER_PID" ]]; then
    kill "$LAUNCH_SERVER_PID" 2>/dev/null || true
fi
if [[ -n "$IMAGE_DECODER_PID" ]]; then
    kill "$IMAGE_DECODER_PID" 2>/dev/null || true
fi
if [[ -n "$PORTAL_PID" ]]; then
    kill "$PORTAL_PID" 2>/dev/null || true
fi
exit "$status"
