#!/usr/bin/env bash
# SDE session supervisor (R4.2, docs/11-r4-implementation-plan.md).
#
# Starts the compositor, launches the shell and the user's XDG Autostart entries,
# and restarts the compositor if it faults (so one crash does not end the
# session). The shell is relaunched by the compositor's own `-s` startup command
# on restart; the compositor reconnects its Wayland clients.
#
# Interoperable by construction (P8/P9): it uses only the XDG Autostart spec and
# the compositor's CLI, and power actions go through logind (`sde-power.sh`).
#
# Environment seams (for embedding and tests):
#   SDE_SESSION_COMPOSITOR   command to run as the compositor (default below)
#   SDE_SESSION_SHELL        shell startup command (only used for the default)
#   SDE_SESSION_AUTOSTART_DIR  autostart dir (default $XDG_CONFIG_HOME/autostart)
#   SDE_SESSION_MAX_RESTARTS   restart budget on non-zero exit (default 3)
#   SDE_SESSION_RESTART_DELAY  seconds between restarts (default 1)
#   SDE_SESSION_AUTOSTART     0 to skip autostart (default 1)
set -u

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

SDE_SESSION_SHELL="${SDE_SESSION_SHELL:-$BUILD/bin/Taskbar & $BUILD/bin/FileManager -d}"
COMPOSITOR="${SDE_SESSION_COMPOSITOR:-$SDE_ROOT/build/WindowServer/WindowServer -n sde -s \"$SDE_SESSION_SHELL\"}"
AUTOSTART_DIR="${SDE_SESSION_AUTOSTART_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/autostart}"
MAX_RESTARTS="${SDE_SESSION_MAX_RESTARTS:-3}"
RESTART_DELAY="${SDE_SESSION_RESTART_DELAY:-1}"
RUN_AUTOSTART="${SDE_SESSION_AUTOSTART:-1}"

# Session environment (expanded in R4.3). Keep it explicit so portals/apps can
# find the session.
export XDG_SESSION_TYPE="${XDG_SESSION_TYPE:-wayland}"
export XDG_CURRENT_DESKTOP="${XDG_CURRENT_DESKTOP:-SDE}"
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    export WAYLAND_DISPLAY="sde"
fi

run_desktop_entry() {
    local entry="$1" line="" exec_line="" hidden="" enabled=""
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in
            Exec=*) exec_line="${line#Exec=}" ;;
            Hidden=*) hidden="${line#Hidden=}" ;;
            X-GNOME-Autostart-enabled=*) enabled="${line#X-GNOME-Autostart-enabled=}" ;;
        esac
    done < "$entry"

    [ "$hidden" = "true" ] && return 0
    [ "$enabled" = "false" ] && return 0
    [ -n "$exec_line" ] || return 0

    # Strip field codes (%u %U %f %F %d %D %n %N %i %c %k %v %m).
    local cmd
    cmd="$(printf '%s' "$exec_line" | sed -E 's/[[:space:]]*%[a-zA-Z]//g')"
    echo "SDE session: autostart $(basename "$entry"): $cmd"
    setsid sh -c "$cmd" >/dev/null 2>&1 &
}

run_autostart() {
    [ "$RUN_AUTOSTART" = "1" ] || return 0
    [ -d "$AUTOSTART_DIR" ] || return 0
    local entry
    for entry in "$AUTOSTART_DIR"/*.desktop; do
        [ -e "$entry" ] || continue
        run_desktop_entry "$entry"
    done
}

run_autostart

restarts=0
while :; do
    echo "SDE session: starting compositor: $COMPOSITOR"
    sh -c "$COMPOSITOR" &
    comp_pid=$!
    # shellcheck disable=SC2064
    trap "kill -TERM $comp_pid 2>/dev/null" TERM INT
    wait "$comp_pid"
    status=$?
    trap - TERM INT

    if [ "$status" -eq 0 ]; then
        echo "SDE session: compositor exited; stopping"
        break
    fi

    restarts=$((restarts + 1))
    if [ "$restarts" -gt "$MAX_RESTARTS" ]; then
        echo "SDE session: compositor failed $restarts time(s); giving up" >&2
        exit 1
    fi
    echo "SDE session: compositor exited ($status); restart $restarts/$MAX_RESTARTS"
    sleep "$RESTART_DELAY"
done
