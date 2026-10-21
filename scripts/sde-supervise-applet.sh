#!/usr/bin/env bash
# Keep an applet running.
#
#   scripts/sde-supervise-applet.sh <applet> [arguments ...]
#
# The session starts applets with `setsid … &` and never looks at them again, so
# an applet that crashes or is killed stays gone until the desktop restarts. For
# the ones a session is not usable without (Wi-Fi, battery) that is the wrong
# default: this wrapper runs the applet and starts it again when it stops.
#
# Deliberately outside every applet: the applets' own code stays untouched, and
# the same wrapper supervises any of them. Restart policy, so a broken applet
# cannot spin:
#
#   * exit status 0 is treated as "it was asked to stop" and ends supervision;
#   * any other exit restarts it after a backoff that doubles from 1s up to 30s;
#   * a run that lasts 60s is considered healthy, and the backoff resets, so an
#     applet that crashes once a day keeps being restarted promptly.
#
# Everything it does is logged to stderr with the applet's name, which the session
# captures in its log; `sde_test`-style tests read those lines.
set -uo pipefail

if [[ $# -lt 1 ]]; then
    echo "usage: sde-supervise-applet.sh <applet> [arguments ...]" >&2
    exit 2
fi

APPLET="$1"
shift
NAME="$(basename "$APPLET")"
delay=1

log() { echo "sde-supervise-applet: $NAME: $*" >&2; }

log "starting"
while true; do
    started="$(date +%s)"
    "$APPLET" "$@"
    status=$?
    ran=$(( $(date +%s) - started ))

    if [[ "$status" -eq 0 ]]; then
        log "exited cleanly after ${ran}s; not restarting"
        exit 0
    fi

    if [[ "$ran" -ge 60 ]]; then
        delay=1
    fi
    log "exited with status ${status} after ${ran}s; restarting in ${delay}s"
    sleep "$delay"
    # Double the wait, up to 30s: a permanently broken applet costs a log line
    # every half minute rather than a hot loop.
    delay=$(( delay * 2 ))
    [[ "$delay" -gt 30 ]] && delay=30
done
