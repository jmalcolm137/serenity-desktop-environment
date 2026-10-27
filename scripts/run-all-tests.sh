#!/usr/bin/env bash
# Run the SDE's test suite, by tier.
#
# Every test is a standalone `scripts/run-*-test.sh`: it builds what it needs,
# runs headless (no DRM, no host compositor), isolates itself in its own runtime
# directory and process group, and cleans up after itself. This driver discovers
# them, runs each with a timeout, records how long it took, and prints a summary —
# it is what CI runs (`.github/workflows/ci.yml`) and what `--tier full` means
# locally.
#
# Tiers. A test opts into the slower suite with a marker comment in its header:
#
#     # ci-tier: full
#
# Unmarked tests are "fast" — short, self-contained, and safe to run on every
# push. Marked tests are heavy, timing-sensitive, or need something a shared CI
# runner does not have (a network service, a portal, `perf_event_open`), so they
# run nightly and on demand. The tier split follows *measured* durations: run
# `--tier all` and read the summary.
#
# Usage:
#   scripts/run-all-tests.sh [--tier fast|full|all] [--only <glob>] [--skip <glob>]
#                            [--timeout <seconds>] [--log-dir <dir>] [--list]
#
#   --tier fast|full|all  which tests to run (default: fast)
#   --only <glob>         restrict to matching names (repeatable)
#   --skip <glob>         exclude matching names (repeatable)
#   --timeout <seconds>   per-test timeout (default: 300)
#   --log-dir <dir>       keep each test's output there (default: a temp dir)
#   --list                print the selection with its tiers and exit
#
# Exit status is non-zero if any test failed or timed out.
#
# Safety: the driver never signals anything but the test it started. Each test
# owns its isolation (its own `setsid` process group and `XDG_RUNTIME_DIR`), and
# its `trap ... EXIT` cleanup runs when the timeout sends it SIGTERM, so a
# timed-out test still takes its compositor down with it. Nothing here matches
# processes by name or command line.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEFAULT_TIMEOUT=300

TIER="fast"
TIMEOUT="$DEFAULT_TIMEOUT"
LOG_DIR=""
LIST_ONLY=0
ONLY=()
SKIP=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tier) TIER="${2:?--tier needs a value}"; shift 2 ;;
        --only) ONLY+=("${2:?--only needs a glob}"); shift 2 ;;
        --skip) SKIP+=("${2:?--skip needs a glob}"); shift 2 ;;
        --timeout) TIMEOUT="${2:?--timeout needs a value}"; shift 2 ;;
        --log-dir) LOG_DIR="${2:?--log-dir needs a path}"; shift 2 ;;
        --list) LIST_ONLY=1; shift ;;
        -h|--help) sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$TIER" in
    fast|full|all) ;;
    *) echo "--tier must be fast, full or all (got '$TIER')" >&2; exit 2 ;;
esac
[[ "$TIMEOUT" =~ ^[0-9]+$ ]] || { echo "--timeout must be a number (got '$TIMEOUT')" >&2; exit 2; }

tier_of() {
    local marker
    marker="$(grep -m1 -E '^# ci-tier:' "$1" 2>/dev/null | sed 's/^# ci-tier:[[:space:]]*//' || true)"
    echo "${marker:-fast}"
}

matches_any() { # $1 = name, rest = globs
    local name="$1" pattern
    shift
    for pattern in "$@"; do
        # shellcheck disable=SC2053 # the glob is intentional
        [[ "$name" == $pattern ]] && return 0
    done
    return 1
}

selected=()
for script in "$SDE_ROOT"/scripts/run-*-test.sh; do
    [[ -f "$script" ]] || continue
    name="$(basename "$script")"
    [[ "$name" == "run-all-tests.sh" ]] && continue
    tier="$(tier_of "$script")"

    case "$TIER" in
        all) ;;
        full) ;;
        fast) [[ "$tier" == "fast" ]] || continue ;;
    esac
    [[ ${#ONLY[@]} -gt 0 ]] && { matches_any "$name" "${ONLY[@]}" || continue; }
    [[ ${#SKIP[@]} -gt 0 ]] && matches_any "$name" "${SKIP[@]}" && continue
    selected+=("$name")
done

if [[ ${#selected[@]} -eq 0 ]]; then
    echo "no tests selected (tier '$TIER')" >&2
    exit 1
fi

if [[ "$LIST_ONLY" -eq 1 ]]; then
    for name in "${selected[@]}"; do
        printf '%s\t%s\n' "$(tier_of "$SDE_ROOT/scripts/$name")" "$name"
    done
    exit 0
fi

keep_logs=1
if [[ -z "$LOG_DIR" ]]; then
    LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sde-all-tests.XXXXXX")"
    keep_logs=0
fi
mkdir -p "$LOG_DIR"

# Runtime state goes on a *short* path, separate from the logs.
#
# A Unix socket address is capped at 108 bytes (sockaddr_un.sun_path). In CI the log
# directory sits under the workspace -- /__w/<repo>/<repo>/ci-logs -- so a session
# directory under it made ".../portal/audiomanager" 122 bytes and every connect to it
# failed with EINVAL ("Could not obtain a sockaddr_un"), which the Audio applet then
# reported as a fatal error. Logs belong with the logs; sockets do not.
RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sde-suite-run.XXXXXX")"
chmod 700 "$RUN_DIR"

# --- refusing to run against a live session ---------------------------------
#
# These tests start compositors, supervisors, portals and services, and several of
# them deliberately drive session-level machinery: the watchdog heartbeat, input
# capture, the session lock ("a wedged compositor must be restarted", "the lock
# engages"). They isolate their own runtime directory, but the *environment* they
# inherit from a session's shell is not theirs to use. On a machine with a live
# session, SDE_HEARTBEAT_FILE points at the live supervisor's liveness file: a test
# that removes or stalls it makes that supervisor decide the live compositor is
# wedged and restart it — taking the session, and everything running in it, down.
# That is exactly how three local runs of this suite killed the session it was
# running inside. So: refuse, unless told explicitly.
live_session_running() {
    local runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    # The session's compositor socket.
    [[ -S "$runtime/${SDE_SESSION_DISPLAY:-sde}" ]] && return 0
    # Or a liveness file written seconds ago. The name carries the session display
    # (sde-heartbeat-<display>), so look for any of them, not just the oldest.
    local heartbeat now modified
    now="$(date +%s)"
    for heartbeat in "$runtime"/sde-heartbeat*; do
        [[ -e "$heartbeat" ]] || continue
        modified="$(stat -c %Y "$heartbeat" 2>/dev/null || echo 0)"
        (( now - modified < 60 )) && return 0
    done
    return 1
}

if live_session_running && [[ "${SDE_ALLOW_LIVE_SESSION:-0}" != 1 ]]; then
    echo "refusing to run: a live SDE session is present on this machine." >&2
    echo "  These tests drive session-level state (the watchdog heartbeat, input" >&2
    echo "  capture, the lock) and can restart the session they are started from." >&2
    echo "  Run them in CI, or on a machine with no session. To override, at your" >&2
    echo "  own risk: SDE_ALLOW_LIVE_SESSION=1 scripts/run-all-tests.sh ..." >&2
    exit 3
fi

# A private runtime directory for the whole run. In a container the one inherited
# from the runner may not exist or may not be writable, and then *every* test's
# compositor fails to create its Wayland socket — the single largest cause of
# failure in the first CI run ("could not add socket 'sde-…'"). The tests use it for
# their sockets and pass the name to their clients, so they need nothing else.
export XDG_RUNTIME_DIR="$RUN_DIR/runtime"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# The backend every test's compositor should use. A CI container has no DRM device
# and no libinput, so a compositor that autoselects the real backend cannot start at
# all ("WindowServer: failed to create wlr_backend", and the test's client then
# "cannot connect to a compositor"). The dev machine *does* have both, which is why
# this only ever failed in CI. A test that wants something else sets it itself.
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=1
# ...and for tests that go through the session's own launcher rather than starting
# the compositor directly (scripts/start-sde-session.sh reads SDE_BACKENDS).
export SDE_BACKENDS="${SDE_BACKENDS:-headless}"

# The SerenityOS resource root every client needs to find its fonts and themes.
# The session exports it to everything it runs, so on a development machine the
# tests inherit it and pass -- but on a clean runner nothing sets it, and a client
# that loads a font then dies in MUST(try_load_from_uri()) with an Illegal
# instruction. Give the whole run one, so a test only has to say nothing.
: "${LIBGUI_WAYLAND_ROOT:=$SDE_ROOT/../libgui-wayland}"
export SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$LIBGUI_WAYLAND_ROOT/serenity/Base/res}"

# Nothing a test starts may reach the live session's files, sockets or services.
# The session exports these to everything it runs — including the shell this driver
# is launched from — so drop them and give the tests their own.
unset SDE_HEARTBEAT_FILE SDE_SESSION_LOG SDE_SESSION_DISPLAY SDE_WM_PORTAL_PATH \
      SDE_STATE_DIR SDE_SESSION_SHELL SDE_SESSION_IDLE_SECONDS \
      SDE_SESSION_WATCHDOG_SECONDS SDE_SESSION_SAFE_MODE SDE_LOCK_CMD \
      SDE_SESSION_IDLE_DPMS_SECONDS SDE_SESSION_COMPOSITOR_SAFE

started_at="$(date +%s)"
passed=0; failed=0; skipped=0; timed_out=0
results=()

echo "=== SDE test suite: tier '$TIER', ${#selected[@]} test(s), ${TIMEOUT}s each ==="
echo

for name in "${selected[@]}"; do
    log="$LOG_DIR/$name.log"
    # Give every test its own session directory. The shim's LibWM *defers* to a
    # process already listening on the session's portal path, so without this a
    # test run on a machine with a live session is answered by that session's
    # services instead of the compositor the test started (the failure mode that
    # made `run-audio-test.sh` red until it isolated itself). Tests that need
    # their own path still set it and win.
    SDE_SESSION_DIR="$RUN_DIR/session-$name"
    # The portal directory has to exist before a service is spawned there: LibWM
    # binds the listening socket for a spawned service (AudioServer, NotificationServer)
    # at "$SDE_SESSION_DIR/portal/<name>" and gives up silently if the bind fails, so a
    # missing directory reads as "the service is not there".
    mkdir -p "$SDE_SESSION_DIR/portal"
    export SDE_SESSION_DIR

    # And no X server. Xwayland's display names live in the global
    # /tmp/.X11-unix, so a test compositor that starts one competes with a live
    # session's — which is how a suite run killed the session's Xwayland and, from
    # there, the session (docs/26-ci.md §6). Only the tests that exercise X11 ask
    # for it back (they unset this and skip when a session is live).
    export SDE_XWAYLAND=0

    # Do not let the session updater create XDG user directories in the
    # developer home while a test runs a session.
    export SDE_USER_DIRS_UPDATE=0

    # And its own liveness heartbeat, so a test that runs a supervisor — or a
    # compositor that stops writing one — can never stall the session's watchdog.
    export SDE_HEARTBEAT_FILE="$RUN_DIR/heartbeat-$name"

    begin="$(date +%s)"
    if timeout -k 10 "$TIMEOUT" bash "$SDE_ROOT/scripts/$name" > "$log" 2>&1; then
        status="PASS"
    else
        rc=$?
        if [[ "$rc" -eq 124 || "$rc" -eq 137 ]]; then
            status="TIMEOUT"
        else
            status="FAIL"
        fi
    fi
    elapsed=$(( $(date +%s) - begin ))

    # The suite reports "SKIP: <reason>" for a test that cannot run here (no
    # compiler, no such hardware). It exits 0, so recognise it by its output.
    if [[ "$status" == "PASS" ]] && grep -qE '^SKIP:|^==> .* SKIPPED' "$log"; then
        status="SKIP"
    fi

    case "$status" in
        PASS) passed=$((passed + 1)) ;;
        SKIP) skipped=$((skipped + 1)) ;;
        TIMEOUT) timed_out=$((timed_out + 1)) ;;
        *) failed=$((failed + 1)) ;;
    esac
    printf '%-8s %4ds  %s\n' "$status" "$elapsed" "$name"
    results+=("$status|$elapsed|$name")
done

total_elapsed=$(( $(date +%s) - started_at ))

echo
echo "=== summary ==="
printf '  passed %d, failed %d, skipped %d, timed out %d (in %ds)\n' \
    "$passed" "$failed" "$skipped" "$timed_out" "$total_elapsed"

slowest="$(printf '%s\n' "${results[@]}" | sort -t'|' -k2 -rn | head -5)"
if [[ -n "$slowest" ]]; then
    echo "  slowest:"
    while IFS='|' read -r status elapsed name; do
        [[ -n "$name" ]] || continue
        printf '    %4ds  %s\n' "$elapsed" "$name"
    done <<< "$slowest"
fi

if [[ "$failed" -gt 0 || "$timed_out" -gt 0 ]]; then
    echo
    echo "  failures (full output in $LOG_DIR):"
    for entry in "${results[@]}"; do
        IFS='|' read -r status elapsed name <<< "$entry"
        [[ "$status" == "PASS" || "$status" == "SKIP" ]] && continue
        echo "    $status  $name"
        tail -8 "$LOG_DIR/$name.log" 2>/dev/null | sed 's/^/      /'
    done
fi

rm -rf "$RUN_DIR"
if [[ "$keep_logs" -eq 0 ]]; then
    if [[ "$failed" -eq 0 && "$timed_out" -eq 0 ]]; then
        rm -rf "$LOG_DIR"
    else
        echo
        echo "  (logs kept in $LOG_DIR)"
    fi
fi

if [[ "$failed" -gt 0 || "$timed_out" -gt 0 ]]; then
    echo "==> test suite FAILED"
    exit 1
fi
echo "==> test suite PASSED"
