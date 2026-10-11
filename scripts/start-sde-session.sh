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

STARTUP="${1:-$BUILD/bin/Taskbar & $BUILD/bin/TextEditor}"

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
SESSION_DISPLAY="${SDE_SESSION_DISPLAY:-sde}"
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
"$SDE_ROOT/scripts/sde-session.sh"
status=$?

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
