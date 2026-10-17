#!/usr/bin/env bash
# The shell launcher: start the Taskbar, and if it dies during startup, fall back
# to a Terminal so the session still has a usable UI.
#
# The compositor's -s startup command cannot tell whether a program mapped a
# window; this wrapper at least catches the case that matters here — the Taskbar
# exits (or crashes) almost immediately. A Taskbar that has stayed up for the
# grace period is considered launched, and the wrapper does not replace it.
#
# Usage: sde-shell.sh [taskbar] [terminal]
set -u

TASKBAR="${1:-${SDE_TASKBAR:-}}"
TERMINAL="${2:-${SDE_TERMINAL:-}}"

if [[ -z "$TASKBAR" || ! -x "$TASKBAR" ]]; then
    echo "sde-shell: no Taskbar at '${TASKBAR:-<unset>}'; starting a Terminal" >&2
    exec "${TERMINAL:-/bin/sh}"
fi

"$TASKBAR" &
taskbar_pid=$!

# Give the Taskbar time to connect, map and survive startup. A process that exits
# in this window did not launch successfully (the common failure is an early
# connection or resource error, not a user action).
for _ in $(seq 1 50); do
    if ! kill -0 "$taskbar_pid" 2>/dev/null; then
        wait "$taskbar_pid" 2>/dev/null
        echo "sde-shell: the Taskbar exited during startup; starting a fallback Terminal" >&2
        if [[ -n "$TERMINAL" && -x "$TERMINAL" ]]; then
            exec "$TERMINAL"
        fi
        exec /bin/sh
    fi
    sleep 0.1
done

# It survived startup: this is the real shell. If it exits later, let the
# session's normal lifecycle handle it rather than spawning a second UI.
wait "$taskbar_pid"
exit $?
