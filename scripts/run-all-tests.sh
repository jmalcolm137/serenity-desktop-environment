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
# A test that has to build an external project first can raise its own limit
# with a second header marker (the global --timeout remains the default):
#
#     # ci-timeout: 1800
#
# A test that measures timing/memory budgets or touches global host state
# (Xwayland, valgrind, sanitizer builds) opts out of parallel execution with:
#
#     # ci-serial: true
#
# Usage:
#   scripts/run-all-tests.sh [--tier fast|full|all] [--only <glob>] [--skip <glob>]
#                            [--jobs <N|auto>] [--no-build]
#                            [--timeout <seconds>] [--log-dir <dir>] [--list]
#
#   --tier fast|full|all  which tests to run (default: fast)
#   --only <glob>         restrict to matching names (repeatable)
#   --skip <glob>         exclude matching names (repeatable)
#   -j, --jobs <N|auto>   max concurrent tests (default: $SDE_TEST_JOBS or 1;
#                         `auto` uses nproc). Tests marked `# ci-serial: true`
#                         always run alone after the parallel pool finishes.
#   --no-build            export SDE_TEST_NO_BUILD=1 so tests skip per-test
#                         build-target.sh calls (use after build-full-sde.sh)
#   --timeout <seconds>   per-test timeout (default: 300)
#   --log-dir <dir>       keep each test's output there (default: a temp dir)
#   --list                print the selection with its tiers and exit
#   --check-guard         report whether a live session is visible (see below)
#                         and exit 0 if running is safe, 3 if not
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
JOBS="${SDE_TEST_JOBS:-1}"
NO_BUILD=0
LOG_DIR=""
LIST_ONLY=0
CHECK_GUARD=0
ONLY=()
SKIP=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --tier) TIER="${2:?--tier needs a value}"; shift 2 ;;
        --only) ONLY+=("${2:?--only needs a glob}"); shift 2 ;;
        --skip) SKIP+=("${2:?--skip needs a glob}"); shift 2 ;;
        -j|--jobs) JOBS="${2:?--jobs needs a value}"; shift 2 ;;
        --no-build) NO_BUILD=1; shift ;;
        --timeout) TIMEOUT="${2:?--timeout needs a value}"; shift 2 ;;
        --log-dir) LOG_DIR="${2:?--log-dir needs a path}"; shift 2 ;;
        --list) LIST_ONLY=1; shift ;;
        --check-guard) CHECK_GUARD=1; shift ;;
        -h|--help) sed -n '2,46p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

case "$TIER" in
    fast|full|all) ;;
    *) echo "--tier must be fast, full or all (got '$TIER')" >&2; exit 2 ;;
esac
[[ "$TIMEOUT" =~ ^[0-9]+$ ]] || { echo "--timeout must be a number (got '$TIMEOUT')" >&2; exit 2; }
if [[ "$JOBS" == "auto" ]]; then
    JOBS="$(nproc 2>/dev/null || echo 1)"
fi
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "--jobs must be a positive integer or 'auto' (got '$JOBS')" >&2; exit 2; }

tier_of() {
    local marker
    marker="$(grep -m1 -E '^# ci-tier:' "$1" 2>/dev/null | sed 's/^# ci-tier:[[:space:]]*//' || true)"
    echo "${marker:-fast}"
}

timeout_of() {
    local marker
    marker="$(grep -m1 -E '^# ci-timeout:' "$1" 2>/dev/null | sed 's/^# ci-timeout:[[:space:]]*//' || true)"
    if [[ "$marker" =~ ^[0-9]+$ ]]; then
        echo "$marker"
    else
        echo "$TIMEOUT"
    fi
}

is_serial() {
    grep -qE '^# ci-serial:[[:space:]]*true([[:space:]]|$)' "$1" 2>/dev/null
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

# Log and runtime directories are created after the live-session guard below:
# --check-guard must not leave anything behind.

# Runtime state goes on a *short* path, separate from the logs.
#
# A Unix socket address is capped at 108 bytes (sockaddr_un.sun_path). In CI the log
# directory sits under the workspace -- /__w/<repo>/<repo>/ci-logs -- so a session
# directory under it made ".../portal/audiomanager" 122 bytes and every connect to it
# failed with EINVAL ("Could not obtain a sockaddr_un"), which the Audio applet then
# reported as a fatal error. Logs belong with the logs; sockets do not.
#
# Created after the live-session guard below: --check-guard must not leave a
# runtime directory behind.

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
# Does a runtime directory hold a live session? The compositor socket, or a
# liveness file written in the last minute.
sde_runtime_has_session() {
    local runtime="$1" heartbeat now modified
    [[ -S "$runtime/${SDE_SESSION_DISPLAY:-sde}" ]] && return 0
    now="$(date +%s)"
    for heartbeat in "$runtime"/sde-heartbeat*; do
        [[ -e "$heartbeat" ]] || continue
        modified="$(stat -c %Y "$heartbeat" 2>/dev/null || echo 0)"
        (( now - modified < 60 )) && return 0
    done
    return 1
}

live_session_running() {
    # An explicit assertion, for a caller that knows better than the probing.
    [[ "${SDE_LIVE_SESSION:-0}" == 1 ]] && return 0
    sde_runtime_has_session "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}" && return 0
    # A container does not inherit the host's /run. The caller can mount it
    # read-only and point here, or name the host's heartbeat directly.
    if [[ -n "${SDE_HOST_RUNTIME_DIR:-}" ]]; then
        sde_runtime_has_session "$SDE_HOST_RUNTIME_DIR" && return 0
    fi
    if [[ -n "${SDE_HOST_HEARTBEAT_FILE:-}" && -e "$SDE_HOST_HEARTBEAT_FILE" ]]; then
        local modified
        modified="$(stat -c %Y "$SDE_HOST_HEARTBEAT_FILE" 2>/dev/null || echo 0)"
        (( $(date +%s) - modified < 60 )) && return 0
    fi
    return 1
}

in_container() {
    # SDE_CONTAINER is the test seam; the marker files cover Docker and Podman.
    [[ "${SDE_CONTAINER:-0}" == 1 ]] && return 0
    [[ -e /.dockerenv || -e /run/.containerenv ]] && return 0
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

# A container cannot see the host's /run, so the check above cannot prove the host
# is idle. Refuse unless the caller makes that explicit: mount the host runtime
# read-only (SDE_HOST_RUNTIME_DIR), name its heartbeat
# (SDE_HOST_HEARTBEAT_FILE), or assert SDE_NO_LIVE_SESSION=1. CI runs in a
# container with no host session and sets the last one.
if [[ "${SDE_ALLOW_LIVE_SESSION:-0}" != 1 ]] && in_container \
    && [[ -z "${SDE_HOST_RUNTIME_DIR:-}" && -z "${SDE_HOST_HEARTBEAT_FILE:-}" \
        && "${SDE_NO_LIVE_SESSION:-0}" != 1 ]]; then
    echo "refusing to run: this is a container and the host's SDE session is not visible." >&2
    echo "  A container does not share the host's /run, so the live-session guard" >&2
    echo "  cannot see a session that is running outside it. Mount the host runtime" >&2
    echo "  read-only and set SDE_HOST_RUNTIME_DIR=/run/user/<uid>, or set" >&2
    echo "  SDE_NO_LIVE_SESSION=1 when the host has no SDE session (CI does)." >&2
    exit 3
fi

if [[ "$CHECK_GUARD" -eq 1 ]]; then
    echo "guard: no live SDE session is visible; running is allowed"
    exit 0
fi

RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sde-suite-run.XXXXXX")"
chmod 700 "$RUN_DIR"

keep_logs=1
if [[ -z "$LOG_DIR" ]]; then
    LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sde-all-tests.XXXXXX")"
    keep_logs=0
fi
mkdir -p "$LOG_DIR"

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

# The tests source scripts/lib/test-env.sh, which applies these same defaults when
# a test is run by hand. Here they are already set (per test, below), so say so.
export SDE_TEST_ENV=runner

if [[ "$NO_BUILD" -eq 1 ]]; then
    export SDE_TEST_NO_BUILD=1
fi

# Nothing a test starts may reach the live session's files, sockets or services.
# The session exports these to everything it runs — including the shell this driver
# is launched from — so drop them and give the tests their own.
unset SDE_HEARTBEAT_FILE SDE_SESSION_LOG SDE_SESSION_DISPLAY LIBWM_WM_PORTAL_PATH \
      SDE_STATE_DIR SDE_SESSION_SHELL SDE_SESSION_IDLE_SECONDS \
      SDE_SESSION_WATCHDOG_SECONDS SDE_SESSION_SAFE_MODE SDE_LOCK_CMD \
      SDE_SESSION_IDLE_DPMS_SECONDS SDE_SESSION_COMPOSITOR_SAFE

# Clean up worker subshells and the temporary runtime dir on Ctrl+C / SIGTERM.
active_pids=()
cleanup_workers() {
    local pid
    for pid in "${active_pids[@]}"; do
        kill -- -"$pid" 2>/dev/null || kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    rm -rf "$RUN_DIR"
    exit 130
}
trap cleanup_workers INT TERM

run_one_test() {
    local idx="$1" name="$2"
    local log="$LOG_DIR/$name.log"
    local res_file="$RUN_DIR/res-$idx"

    # Give every test its own short private runtime, session, state and
    # heartbeat paths so concurrent tests cannot collide on Wayland sockets,
    # theme/font brokers or portal directories (and socket paths stay well
    # under the 108-byte sockaddr_un.sun_path limit).
    local test_runtime="$RUN_DIR/r-$idx"
    local test_session="$RUN_DIR/s-$idx"
    local test_state="$RUN_DIR/st-$idx"
    local test_heartbeat="$RUN_DIR/h-$idx"
    mkdir -p "$test_runtime" "$test_session/portal" "$test_state"
    chmod 700 "$test_runtime"

    local begin elapsed status rc
    begin="$(date +%s)"
    if XDG_RUNTIME_DIR="$test_runtime" \
        LIBWM_SESSION_DIR="$test_session" \
        SDE_STATE_DIR="$test_state" \
        SDE_HEARTBEAT_FILE="$test_heartbeat" \
        SDE_XWAYLAND=0 \
        SDE_USER_DIRS_UPDATE=0 \
        SDE_XDG_DATA_UPDATE=0 \
        timeout -k 10 "$(timeout_of "$SDE_ROOT/scripts/$name")" bash "$SDE_ROOT/scripts/$name" > "$log" 2>&1; then
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

    printf '%s|%d|%s\n' "$status" "$elapsed" "$name" > "$res_file"
    printf '%-8s %4ds  %s\n' "$status" "$elapsed" "$name"
}

started_at="$(date +%s)"
passed=0; failed=0; skipped=0; timed_out=0
results=()

parallel_idxs=()
serial_idxs=()
for idx in "${!selected[@]}"; do
    name="${selected[$idx]}"
    if [[ "$JOBS" -gt 1 ]] && ! is_serial "$SDE_ROOT/scripts/$name"; then
        parallel_idxs+=("$idx")
    else
        serial_idxs+=("$idx")
    fi
done

echo "=== SDE test suite: tier '$TIER', ${#selected[@]} test(s), jobs=$JOBS, ${TIMEOUT}s default per test ==="
echo

if [[ ${#parallel_idxs[@]} -gt 0 ]]; then
    running=0
    for idx in "${parallel_idxs[@]}"; do
        name="${selected[$idx]}"
        (
            run_one_test "$idx" "$name"
        ) &
        active_pids+=("$!")
        running=$((running + 1))
        if [[ "$running" -ge "$JOBS" ]]; then
            wait -n 2>/dev/null || true
            running=$((running - 1))
        fi
    done
    wait 2>/dev/null || true
    active_pids=()
fi

for idx in "${serial_idxs[@]}"; do
    run_one_test "$idx" "${selected[$idx]}"
done

trap - INT TERM

for idx in "${!selected[@]}"; do
    res_file="$RUN_DIR/res-$idx"
    if [[ -f "$res_file" ]]; then
        entry="$(<"$res_file")"
    else
        entry="FAIL|0|${selected[$idx]}"
    fi
    IFS='|' read -r status elapsed name <<< "$entry"
    case "$status" in
        PASS) passed=$((passed + 1)) ;;
        SKIP) skipped=$((skipped + 1)) ;;
        TIMEOUT) timed_out=$((timed_out + 1)) ;;
        *) failed=$((failed + 1)) ;;
    esac
    results+=("$entry")
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
