#!/usr/bin/env bash
# The shell launcher: start the Taskbar and keep it running.
#
# The compositor's -s startup command cannot tell whether a program mapped a
# window, so the wrapper has two jobs:
#
#   * Startup. A Taskbar that exits (or crashes) within the grace period did not
#     launch successfully; start a Terminal so the session still has a usable UI
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
#   SDE_SHELL_MAX_RESTARTS    restarts after startup (default 5)
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

start_taskbar

# Give the Taskbar time to connect, map and survive startup. A process that exits
# in this window did not launch successfully (the common failure is an early
# connection or resource error, not a user action).
for _ in $(seq 1 "$STARTUP_CHECKS"); do
    if ! kill -0 "$taskbar_pid" 2>/dev/null; then
        wait "$taskbar_pid" 2>/dev/null
        taskbar_pid=""
        echo "sde-shell: the Taskbar exited during startup; starting a fallback Terminal" >&2
        fallback_terminal
    fi
    sleep 0.1
done

# It survived startup: this is the real shell. Keep it that way.
echo "sde-shell: the Taskbar (pid $taskbar_pid) survived startup and is the shell" >&2
restarts=0
while :; do
    wait "$taskbar_pid"
    status=$?
    taskbar_pid=""
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
