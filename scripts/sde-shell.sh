#!/usr/bin/env bash
# The shell launcher: start the Taskbar and keep it running.
#
# The compositor's -s startup command cannot tell whether a program mapped a
# window, so the wrapper has two jobs:
#
#   * Startup. A Taskbar that exits within the grace period is retried the same
#     way as a later exit. A startup exit is not proof that the Taskbar is
#     broken: the Taskbar exits when its LaunchServer connection dies, and the
#     service supervisor restarts LaunchServer. Only a Taskbar that cannot stay
#     up at all falls back to a Terminal, so the session still has a usable UI
#     rather than an empty screen with applets.
#   * Steady state. A Taskbar that works and then exits is restarted with a
#     bounded backoff. Before this, a later exit (for example, its LaunchServer
#     connection dying) left the session with no panel until logout, because the
#     session's compositor-restart path does not relaunch the shell.
#
# Usage: sde-shell.sh [taskbar] [terminal]
#
# Environment seams (for tests and embedding):
#   SDE_SHELL_STARTUP_CHECKS  startup grace in 0.1 s ticks (default 50, i.e. 5 s)
#   SDE_SHELL_MAX_RESTARTS    restarts before the Terminal fallback (default 5)
#   SDE_SHELL_RESTART_DELAY   base backoff in seconds (default 1; capped at 5)
set -u

TASKBAR="${1:-${SDE_TASKBAR:-}}"
TERMINAL="${2:-${SDE_TERMINAL:-}}"
STARTUP_CHECKS="${SDE_SHELL_STARTUP_CHECKS:-50}"
MAX_RESTARTS="${SDE_SHELL_MAX_RESTARTS:-5}"
RESTART_DELAY="${SDE_SHELL_RESTART_DELAY:-1}"

taskbar_pid=""
stopping=0

start_taskbar() {
    "$TASKBAR" &
    taskbar_pid=$!
}

fallback_terminal() {
    if [[ -n "$TERMINAL" && -x "$TERMINAL" ]]; then
        exec "$TERMINAL"
    fi
    exec /bin/sh
}

# The compositor stops the shell by sweeping its process group. Take the Taskbar
# with us instead of leaving it orphaned (and do not restart it on the way out).
# shellcheck disable=SC2064
trap 'stopping=1; if [[ -n "$taskbar_pid" ]]; then kill -TERM "$taskbar_pid" 2>/dev/null || true; fi; exit 0' TERM INT

if [[ -z "$TASKBAR" || ! -x "$TASKBAR" ]]; then
    echo "sde-shell: no Taskbar at '${TASKBAR:-<unset>}'; starting a Terminal" >&2
    fallback_terminal
fi

# Wait out the startup grace. Return 1 if the Taskbar exits during it; the exit
# status is left in startup_status.
startup_grace() {
    for _ in $(seq 1 "$STARTUP_CHECKS"); do
        if ! kill -0 "$taskbar_pid" 2>/dev/null; then
            wait "$taskbar_pid" 2>/dev/null
            startup_status=$?
            taskbar_pid=""
            return 1
        fi
        sleep 0.1
    done
}

start_taskbar

restarts=0
while :; do
    if startup_grace; then
        # It survived startup: this is the real shell. Wait for it to exit.
        echo "sde-shell: the Taskbar (pid $taskbar_pid) survived startup and is the shell" >&2
        wait "$taskbar_pid"
        status=$?
        taskbar_pid=""
    else
        # A startup exit may be transient (the LaunchServer it depends on is
        # being restarted), so retry instead of abandoning the panel.
        status=$startup_status
        echo "sde-shell: the Taskbar exited during startup (status $status)" >&2
    fi
    [[ "$stopping" -eq 1 ]] && exit 0

    restarts=$((restarts + 1))
    if [[ "$restarts" -gt "$MAX_RESTARTS" ]]; then
        echo "sde-shell: the Taskbar exited $restarts times; starting a fallback Terminal" >&2
        fallback_terminal
    fi

    delay=$((RESTART_DELAY * restarts))
    [[ "$delay" -gt 5 ]] && delay=5
    echo "sde-shell: the Taskbar exited (status $status); restarting ($restarts/$MAX_RESTARTS) in ${delay}s" >&2
    sleep "$delay"
    [[ "$stopping" -eq 1 ]] && exit 0
    start_taskbar
done
