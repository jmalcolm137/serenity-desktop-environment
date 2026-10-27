#!/usr/bin/env bash
# Keep one or more applets running.
#
#   scripts/sde-supervise-applet.sh <applet> [arguments ...]
#   scripts/sde-supervise-applet.sh --applet "<name>|<command>" [--applet ...]
#
# The session starts applets with `setsid … &` and never looks at them again, so
# an applet that crashes or is killed stays gone until the desktop restarts. For
# the ones a session is not usable without (Wi-Fi, battery) that is the wrong
# default: this wrapper runs the applets and starts them again when they stop.
#
# A single process supervises the whole set (scripts/sde-applets.sh builds the
# `--applet` specs), so the session carries one small helper instead of one Bash
# process per applet. The single-applet form is the same script, for a launcher
# or a test that wants only one.
#
# Deliberately outside every applet: the applets' own code stays untouched, and
# the same wrapper supervises any of them. Restart policy, so a broken applet
# cannot spin:
#
#   * exit status 0 is treated as "it was asked to stop"; that applet is not
#     restarted, and the supervisor exits once every applet has stopped this way;
#   * any other exit restarts it after a backoff that doubles from 1s up to 30s;
#   * a run that lasts 60s is considered healthy, and the backoff resets, so an
#     applet that crashes once a day keeps being restarted promptly.
#
# Each backoff runs in its own timer process, so one applet waiting 30s does not
# delay another applet's restart.
#
# Everything it does is logged to stderr with the applet's name, which the session
# captures in its log; `sde_test`-style tests read those lines.
set -uo pipefail

usage() {
    echo "usage: sde-supervise-applet.sh <applet> [arguments ...]" >&2
    echo "       sde-supervise-applet.sh --applet \"<name>|<command>\" [--applet ...]" >&2
    exit 2
}

NAMES=()
COMMANDS=()
DELAYS=()
STARTED_AT=()
INDEX_OF_PID=()
TIMER_INDEX=()

if [[ "${1:-}" == "--applet" ]]; then
    while [[ $# -gt 0 ]]; do
        [[ "$1" == "--applet" ]] || usage
        shift
        [[ $# -gt 0 ]] || usage
        spec="$1"
        shift

        name="${spec%%|*}"
        command="${spec#*|}"
        [[ -n "$name" && "$command" != "$spec" ]] || usage

        NAMES+=("$name")
        COMMANDS+=("$command")
        DELAYS+=(1)
        STARTED_AT+=(0)
    done
else
    [[ $# -ge 1 ]] || usage
    NAMES+=("$(basename "$1")")
    COMMANDS+=("$(printf '%q ' "$@")")
    DELAYS+=(1)
    STARTED_AT+=(0)
fi

log() { echo "sde-supervise-applet: $1: $2" >&2; }

start_applet() {
    local index="$1"
    log "${NAMES[$index]}" "starting"

    # The spec's command was built with printf %q, so re-splitting it here yields
    # the original argv without a shell or a second round of word splitting.
    eval "set -- ${COMMANDS[$index]}"
    "$@" &
    local pid=$!
    INDEX_OF_PID[$pid]="$index"
    STARTED_AT[$index]="$(date +%s)"
}

schedule_restart() {
    local index="$1" delay="$2"
    ( sleep "$delay"; exit 0 ) &
    local timer_pid=$!
    TIMER_INDEX[$timer_pid]="$index"
}

stopping=0
cleanup() {
    stopping=1
    local pid
    for pid in "${!INDEX_OF_PID[@]}"; do
        [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null || true
    done
    for pid in "${!TIMER_INDEX[@]}"; do
        [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    exit 0
}
# shellcheck disable=SC2064
trap cleanup TERM INT

active="${#NAMES[@]}"
index=0
while [[ "$index" -lt "${#NAMES[@]}" ]]; do
    start_applet "$index"
    index=$((index + 1))
done

while [[ "$active" -gt 0 ]]; do
    finished_pid=""
    wait -n -p finished_pid
    status=$?
    [[ "$stopping" -eq 1 ]] && exit 0
    [[ -n "$finished_pid" ]] || continue

    if [[ -n "${TIMER_INDEX[$finished_pid]:-}" ]]; then
        index="${TIMER_INDEX[$finished_pid]}"
        unset 'TIMER_INDEX[$finished_pid]'
        start_applet "$index"
        continue
    fi

    index="${INDEX_OF_PID[$finished_pid]:-}"
    if [[ -z "$index" ]]; then
        # A child this supervisor did not start (or one already reaped): ignore.
        continue
    fi
    unset 'INDEX_OF_PID[$finished_pid]'

    ran=$(( $(date +%s) - STARTED_AT[$index] ))
    if [[ "$status" -eq 0 ]]; then
        log "${NAMES[$index]}" "exited cleanly after ${ran}s; not restarting"
        active=$((active - 1))
        continue
    fi

    if [[ "$ran" -ge 60 ]]; then
        DELAYS[$index]=1
    fi
    delay="${DELAYS[$index]}"
    log "${NAMES[$index]}" "exited with status ${status} after ${ran}s; restarting in ${delay}s"
    schedule_restart "$index" "$delay"
    delay=$(( delay * 2 ))
    [[ "$delay" -gt 30 ]] && delay=30
    DELAYS[$index]="$delay"
done
