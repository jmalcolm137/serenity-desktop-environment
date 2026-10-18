#!/usr/bin/env bash
# Supervise one long-running session service.
#
# The session starts LaunchServer, ImageDecoder and the portal backend as plain
# background jobs. A crash used to leave that service dead for the rest of the
# session (docs/07-systemserver-roles.md §4: "No supervisor / restart"). This
# wrapper restarts it with a bounded budget and a short backoff, and stops the
# child cleanly when the session stops the wrapper.
#
# Usage: sde-service-supervisor.sh <name> -- <command> [args...]
#
# Environment seams (for tests and embedding):
#   SDE_SERVICE_MAX_RESTARTS   restarts before giving up (default 5)
#   SDE_SERVICE_RESTART_DELAY  base backoff in seconds (default 1; capped at 5)
set -u

NAME="${1:-service}"
shift || true
[[ "${1:-}" == "--" ]] && shift

if [[ $# -eq 0 ]]; then
    echo "usage: sde-service-supervisor.sh <name> -- <command> [args...]" >&2
    exit 2
fi

MAX_RESTARTS="${SDE_SERVICE_MAX_RESTARTS:-5}"
RESTART_DELAY="${SDE_SERVICE_RESTART_DELAY:-1}"

child_pid=""
stopping=0

# shellcheck disable=SC2064
trap 'stopping=1; if [[ -n "$child_pid" ]]; then kill -TERM "$child_pid" 2>/dev/null || true; fi; exit 0' TERM INT

restarts=0
while :; do
    "$@" &
    child_pid=$!
    wait "$child_pid"
    status=$?
    child_pid=""
    [[ "$stopping" -eq 1 ]] && exit 0

    restarts=$((restarts + 1))
    if [[ "$restarts" -gt "$MAX_RESTARTS" ]]; then
        echo "$NAME: exited $restarts times; not restarting" >&2
        exit "$status"
    fi

    delay=$((RESTART_DELAY * restarts))
    [[ "$delay" -gt 5 ]] && delay=5
    echo "$NAME: exited (status $status); restarting ($restarts/$MAX_RESTARTS) in ${delay}s" >&2
    sleep "$delay"
done
