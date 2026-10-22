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
#   The startup command defaults to running the Taskbar and a File Manager.
#
# Set SDE_BACKENDS to force a backend list (passed to wlr as WLR_BACKENDS), e.g.
# SDE_BACKENDS=headless ./start-sde-session.sh
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="${SDE_COMPOSITOR_BIN:-$SDE_ROOT/build/WindowServer/WindowServer}"

: "${XDG_RUNTIME_DIR:?XDG_RUNTIME_DIR must be set (log in to a proper session)}"

# A display manager (SDDM) discards the session's stdout, so when there is no
# terminal, log to a file. Diagnose a failed login with:
#   cat ~/.local/state/sde/session.log
#
# The log is capped. It carries every client's diagnostics, and a single chatty
# process is enough to fill the disk: a terminal logging an unimplemented escape
# mode and the shim logging every present grew one session's log past a gigabyte.
# Rotate in place — the writers hold the fd with O_APPEND, so truncating the same
# inode is safe and keeps them logging — and keep the tail beside it.
if [[ ! -t 1 ]]; then
    SDE_SESSION_LOG="${SDE_SESSION_LOG:-${XDG_STATE_HOME:-${HOME:-/tmp}/.local/state}/sde/session.log}"
    : "${SDE_SESSION_LOG_MAX_KB:=65536}"  # 64 MiB before rotating
    : "${SDE_SESSION_LOG_KEEP_KB:=8192}"  # tail preserved as session.log.1
    if mkdir -p "$(dirname "$SDE_SESSION_LOG")" 2>/dev/null && : >>"$SDE_SESSION_LOG" 2>/dev/null; then
        exec >>"$SDE_SESSION_LOG" 2>&1
        echo "--- SDE session start $(date -Is) ---"
        echo "user=$(id -un) uid=$(id -u) display=${WAYLAND_DISPLAY:-<unset>} session_type=${XDG_SESSION_TYPE:-<unset>}"
        echo "seat=${XDG_SEAT:-<unset>} vt=${XDG_VTNR:-<unset>} runtime=${XDG_RUNTIME_DIR}"

        (
            while :; do
                sleep 20
                size_kb="$(du -k "$SDE_SESSION_LOG" 2>/dev/null | cut -f1)" || continue
                [[ -n "$size_kb" && "$size_kb" -gt "$SDE_SESSION_LOG_MAX_KB" ]] || continue
                tail -c "$((SDE_SESSION_LOG_KEEP_KB * 1024))" "$SDE_SESSION_LOG" > "$SDE_SESSION_LOG.1" 2>/dev/null || true
                truncate -s 0 "$SDE_SESSION_LOG" 2>/dev/null || true
                echo "--- SDE session log rotated (> ${SDE_SESSION_LOG_MAX_KB} KiB; the tail is in ${SDE_SESSION_LOG##*/}.1) ---"
            done
        ) &
    fi
fi

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
# Only a non-default seat relocates the WindowManagerServer socket; seat0 uses the
# default (/tmp/portal/wm), which the compositor creates. Exporting the default
# would make the compositor skip creating /tmp/portal (see WindowServer main.cpp).
if [[ -z "${SDE_WM_PORTAL_PATH:-}" && -n "$(sde_seat_suffix)" ]]; then
    SDE_WM_PORTAL_PATH="$(sde_wm_portal_path)"
    export SDE_WM_PORTAL_PATH
fi
echo "SDE: seat '${XDG_SEAT}'${XDG_VTNR:+ vt$XDG_VTNR} display '$SDE_SESSION_DISPLAY' wm-portal '${SDE_WM_PORTAL_PATH:-/tmp/portal/wm}'"

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
# A default wallpaper for a fresh state directory: the compositor seeds from these
# on first run, after which the state set via Display Settings is authoritative.
export SDE_WALLPAPER="${SDE_WALLPAPER:-blue-grid.png}"
export SDE_WALLPAPER_MODE="${SDE_WALLPAPER_MODE:-fill}"
export LD_LIBRARY_PATH="$BUILD/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# A framework rebuild (libgui-wayland) relinks liblagom-* and changes headers the
# SDE's own programs compile against. A stale mix is not benign: when LibIPC's
# ConnectionBase gained a member, an old LaunchServer read m_deferred_invoker at
# the wrong offset, crashed on its first client disconnect, and the Taskbar --
# whose LibDesktop connection exits the app when the server goes away -- vanished
# from the panel. Rebuild the SDE's programs when the framework is newer, so a
# login after a framework-only rebuild cannot start a mismatched session.
framework_is_newer_than_sde() {
    local binary library
    for binary in \
        "$COMP" \
        "$BUILD/bin/Taskbar" \
        "$BUILD/bin/Terminal" \
        "$BUILD/bin/FileManager" \
        "$BUILD/bin/DisplaySettings" \
        "$SDE_ROOT/build/LaunchServer/LaunchServer" \
        "$SDE_ROOT/build/ImageDecoder/ImageDecoder" \
        "$SDE_ROOT/build/AudioServer/AudioServer" \
        "$SDE_ROOT/build/Applets/Audio" \
        "$SDE_ROOT/build/BatteryApplet/BatteryApplet" \
        "$SDE_ROOT/build/WifiApplet/WifiApplet"; do
        [ -x "$binary" ] || continue
        # Only the framework libraries this program actually links matter: a
        # relinked LibVT does not make the compositor stale. ldd resolves the
        # soname symlinks to the real files ninja relinks.
        while IFS= read -r library; do
            [ -e "$library" ] && [ "$library" -nt "$binary" ] && return 0
        done < <(ldd "$binary" 2>/dev/null | awk '$2 == "=>" && $3 ~ /\/lib(lagom-|windowserver)/ { print $3 }')
    done
    return 1
}

if [[ -x "$SDE_ROOT/scripts/build-target.sh" ]] && framework_is_newer_than_sde; then
    echo "SDE: the framework is newer than the session's programs; rebuilding them"
    if ! "$SDE_ROOT/scripts/build-target.sh"; then
        echo "SDE: warning: the rebuild failed; continuing with the existing programs" >&2
    fi
fi

# The applications run from this build's bin directory; the session LaunchServer
# (below) is not built next to them, so point it there. See
# docs/06-service-layering.md.
export SDE_APP_BIN_DIR="${SDE_APP_BIN_DIR:-$BUILD/bin}"

# R5 accessibility: make the session's AT-SPI bus visible to every client. This
# must happen before the services below start, because the LaunchServer captures
# its environment and hands it to the applications it spawns.
# shellcheck source=scripts/sde-accessibility.sh
. "$SDE_ROOT/scripts/sde-accessibility.sh"
sde_export_at_spi_bus

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

# Start the Taskbar and a File Manager, plus the taskbar applets, unless the
# caller supplies their own startup command.
# Every applet runs under a supervising wrapper, so one that dies comes back
# without the applets themselves knowing anything about it.
SUPERVISE="$SDE_ROOT/scripts/sde-supervise-applet.sh"
BATTERY_APPLET="${SDE_BATTERY_APPLET:-$SDE_ROOT/build/BatteryApplet/BatteryApplet}"
if [[ ! -x "$BATTERY_APPLET" && -x "$SDE_APP_BIN_DIR/BatteryApplet" ]]; then
    BATTERY_APPLET="$SDE_APP_BIN_DIR/BatteryApplet"
fi
if [[ ! -x "$BATTERY_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-battery-applet.sh" >/dev/null 2>&1 \
        || echo "note: BatteryApplet not built; skipping the battery applet" >&2
fi

# The Wi-Fi applet (a LibGUI app that fronts NetworkManager; docs/19).
WIFI_APPLET="${SDE_WIFI_APPLET:-$SDE_ROOT/build/WifiApplet/WifiApplet}"
if [[ ! -x "$WIFI_APPLET" && -x "$SDE_APP_BIN_DIR/WifiApplet" ]]; then
    WIFI_APPLET="$SDE_APP_BIN_DIR/WifiApplet"
fi
if [[ ! -x "$WIFI_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-wifi-applet.sh" >/dev/null 2>&1 \
        || echo "note: WifiApplet not built; skipping the Wi-Fi applet" >&2
fi

# The SerenityOS taskbar applets (WorkspacePicker, ResourceGraph, Network, ...)
# are SDE's; build them on demand so a fresh checkout gets them too. An installed
# tree sets SDE_APPLET_DIR (the flattened bin directory).
APPLET_DIR="${SDE_APPLET_DIR:-$SDE_ROOT/build/Applets}"
if [[ ! -x "$APPLET_DIR/WorkspacePicker" ]]; then
    "$SDE_ROOT/scripts/build-applets.sh" >/dev/null 2>&1 \
        || echo "note: applets not built; skipping them" >&2
fi
# shellcheck source=scripts/sde-applets.sh
. "$SDE_ROOT/scripts/sde-applets.sh"
APPLET_CMDS="$(sde_applet_startup_commands "$SDE_ROOT" "$SUPERVISE")"

# R6/N4: a minimal startup for safe mode (used automatically when the compositor
# keeps failing, and with SDE_SAFE_MODE=1): just the shell, no applets/battery.
# The wrapper falls back to a Terminal if the Taskbar does not survive startup.
SHELL_WRAPPER="$SDE_ROOT/scripts/sde-shell.sh"
SAFE_STARTUP="$SHELL_WRAPPER $BUILD/bin/Taskbar $BUILD/bin/Terminal"

if [[ "${SDE_SAFE_MODE:-0}" == "1" ]]; then
    # Manual safe mode: minimal shell, and tell the supervisor to skip autostart.
    STARTUP="$SAFE_STARTUP"
    export SDE_SESSION_SAFE_MODE=1
    # The wrapper's Terminal is a fallback, not an unconditional startup app.
    export SDE_SESSION_STARTUP_APPS="Taskbar"
elif [[ -n "${1:-}" ]]; then
    STARTUP="$1"
else
    STARTUP="$SHELL_WRAPPER $BUILD/bin/Taskbar $BUILD/bin/Terminal & $BUILD/bin/FileManager -d"
    # The wrapper runs the Taskbar and falls back to Terminal; the Terminal is not
    # unconditional, so a Terminal the user had open must still be restored.
    export SDE_SESSION_STARTUP_APPS="Taskbar FileManager"
    if [[ -n "$APPLET_CMDS" ]]; then
        STARTUP="$STARTUP & $APPLET_CMDS"
    fi
    if [[ -x "$BATTERY_APPLET" ]]; then
        STARTUP="$STARTUP & $SUPERVISE $BATTERY_APPLET"
    fi
    if [[ -x "$WIFI_APPLET" ]]; then
        STARTUP="$STARTUP & $SUPERVISE $WIFI_APPLET"
    fi
fi

# The supervisor uses the actual startup command for its restore heuristic (and
# for the default compositor command); without this a custom startup would be
# parsed as the default one.
export SDE_SESSION_SHELL="$STARTUP"

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
LAUNCH_SERVER="${SDE_LAUNCH_SERVER_BIN:-$SDE_ROOT/build/LaunchServer/LaunchServer}"
LAUNCH_SERVER_PID=""
SERVICE_SUPERVISOR="$SDE_ROOT/scripts/sde-service-supervisor.sh"
# Drop the previous session's Xwayland DISPLAY so the LaunchServer waits for the
# fresh one the compositor writes on startup (else it would use a stale or host
# DISPLAY for the X11 apps it spawns).
rm -f "${SDE_STATE_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/sde}/xwayland-display"
if [[ -x "$LAUNCH_SERVER" ]]; then
    # Long-running session services are supervised (R6/N4): a crash is restarted
    # with a bounded backoff instead of staying dead for the rest of the session.
    WAYLAND_DISPLAY="$SESSION_DISPLAY" "$SERVICE_SUPERVISOR" LaunchServer -- "$LAUNCH_SERVER" &
    LAUNCH_SERVER_PID=$!
fi

# The session ImageDecoder owns the image portal when it is built and running;
# without it, applications fall back to the in-process ImageDecoder inside
# libgui-wayland. Either way image decoding works.
IMAGE_DECODER="${SDE_IMAGE_DECODER_BIN:-$SDE_ROOT/build/ImageDecoder/ImageDecoder}"
IMAGE_DECODER_PID=""
if [[ -x "$IMAGE_DECODER" ]]; then
    "$SERVICE_SUPERVISOR" ImageDecoder -- "$IMAGE_DECODER" &
    IMAGE_DECODER_PID=$!
fi

# The SDE portal backend (R5): xdg-desktop-portal-serenity implements the
# org.freedesktop.impl.portal.* interfaces (Settings, ScreenCast, RemoteDesktop,
# FileChooser, Screenshot, Print, ...) that portals.conf selects, so
# xdg-desktop-portal (and thus GTK/Qt/Flatpak apps) follow the SDE session.
# Falls back to the R4.4 Settings-only backend; harmless when xdg-desktop-portal
# is absent.
PORTAL_BACKEND="${SDE_PORTAL_BACKEND_BIN:-$SDE_ROOT/build/tools/xdg-desktop-portal-serenity}"
SETTINGS_PORTAL="${SDE_SETTINGS_PORTAL_BIN:-$SDE_ROOT/build/tools/sde-settings-portal}"
PORTAL_PID=""
# A second session on the same bus cannot own the portal's D-Bus name. Tests and
# nested sessions can opt out; the normal session leaves this at 1.
if [[ "${SDE_SESSION_PORTAL:-1}" == "1" ]]; then
    if [[ -x "$PORTAL_BACKEND" ]]; then
        "$SERVICE_SUPERVISOR" PortalBackend -- "$PORTAL_BACKEND" &
        PORTAL_PID=$!
    elif [[ -x "$SETTINGS_PORTAL" ]]; then
        "$SERVICE_SUPERVISOR" SettingsPortal -- "$SETTINGS_PORTAL" &
        PORTAL_PID=$!
    fi
fi

# PolicyKit authentication agent: the SDE-owned prompt (and the "do not ask
# again this session" cache). The trusted polkit helper still performs PAM.
POLKIT_AGENT="${SDE_POLKIT_AGENT:-$SDE_ROOT/scripts/sde-polkit-agent.py}"
AUTH_DIALOG="${SDE_AUTH_DIALOG:-$SDE_ROOT/build/AuthDialog/sde-auth-dialog}"
[[ -x "$AUTH_DIALOG" ]] || AUTH_DIALOG="$BUILD/bin/sde-auth-dialog"
POLKIT_AGENT_PID=""
if [[ -x "$POLKIT_AGENT" && -x "$AUTH_DIALOG" ]]; then
    SDE_AUTH_DIALOG="$AUTH_DIALOG" "$POLKIT_AGENT" &
    POLKIT_AGENT_PID=$!
fi

echo "SDE: starting WindowServer (${SDE_BACKENDS:-drm,libinput}) on $XDG_RUNTIME_DIR"
# Build the portable session tools (sde-idle) if needed.
if [ ! -x "${SDE_SESSION_IDLE_BIN:-$SDE_ROOT/build/tools/sde-idle}" ]; then
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
if [[ -n "$POLKIT_AGENT_PID" ]]; then
    kill "$POLKIT_AGENT_PID" 2>/dev/null || true
fi
exit "$status"
