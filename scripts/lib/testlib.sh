#!/usr/bin/env bash
# Shared setup and teardown for the headless test scripts.
#
# Every scripts/run-*-test.sh used to repeat the same ~40 lines: resolve the
# repository roots, build the compositor, make a private runtime directory, start
# the compositor with the headless backend and wait for its socket, remember to
# kill it (and the clients) and remove the socket on every exit path, and count
# the failures. This is that code, once:
#
#   SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
#   . "$SDE_ROOT/scripts/lib/testlib.sh"
#   sde_test_init virtual-touch
#   sde_test_build WindowServer
#   sde_test_compositor
#   sde_test_client env WAYLAND_DISPLAY="$SOCK" "$client"
#   sde_test_check "$RUNTIME/comp.log" 'drag started' "a drag started"
#   sde_test_finish
#
# The caller still owns the test's own assertions and its clients; the library
# owns the process lifecycle, the directories and the pass/fail bookkeeping, so a
# test that forgets a cleanup path cannot leak a compositor or a socket into the
# next run. Sourcing it does not change the shell's options: a script keeps its
# own `set -euo pipefail`.
#
# Names are prefixed SDE_TEST_ (and the functions sde_test_*) so a script can
# keep using its own variables.

# --- Setup ------------------------------------------------------------------

# sde_test_init <name> [runtime-prefix]
#   Resolves the roots the tests share, makes a private session directory, and
#   arranges for sde_test_cleanup to run on exit (and on a signal).
sde_test_init() {
    SDE_TEST_NAME="${1:?sde_test_init needs a test name}"
    local prefix="${2:-sde-$SDE_TEST_NAME}"

    SDE_TEST_SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
    SDE_TEST_LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_TEST_SDE_ROOT/../libgui-wayland}"
    SDE_TEST_SERENITY="${SERENITY:-$SDE_TEST_LIBGUI_WAYLAND_ROOT/serenity}"
    SDE_TEST_BUILD="${BUILD:-$SDE_TEST_LIBGUI_WAYLAND_ROOT/build/lagom}"
    SDE_TEST_COMPOSITOR="$SDE_TEST_SDE_ROOT/build/WindowServer/WindowServer"
    export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/$prefix.XXXXXX")"
    chmod 700 "$RUNTIME"
    export SDE_STATE_DIR="$RUNTIME"
    SOCK="$prefix-$$"

    SDE_TEST_COMPOSITOR_PID=""
    SDE_TEST_CLIENT_PIDS=()
    SDE_TEST_FAILURES=0
    SDE_TEST_FINISHED=0

    trap sde_test_cleanup EXIT
    trap 'sde_test_cleanup; exit 130' INT TERM
}

# The names the tests were written against. Kept as variables (not just
# SDE_TEST_*) so a migrated test reads the same as an unmigrated one.
sde_test_export_roots() {
    SDE_ROOT="$SDE_TEST_SDE_ROOT"
    LIBGUI_WAYLAND_ROOT="$SDE_TEST_LIBGUI_WAYLAND_ROOT"
    SERENITY="$SDE_TEST_SERENITY"
    BUILD="$SDE_TEST_BUILD"
    COMP="$SDE_TEST_COMPOSITOR"
}

# --- Build ------------------------------------------------------------------

# sde_test_build [target ...]
#   Builds through the SDE's single build tree (scripts/build-target.sh). With no
#   target, the compositor and the test clients, which every test needs.
sde_test_build() {
    if [[ $# -eq 0 ]]; then
        "$SDE_TEST_SDE_ROOT/scripts/build-target.sh" WindowServer sde-tools >/dev/null
    else
        "$SDE_TEST_SDE_ROOT/scripts/build-target.sh" "$@" >/dev/null
    fi
}

# sde_test_vi: the path to the virtual-input harness (builds it if needed).
sde_test_vi() {
    "$SDE_TEST_SDE_ROOT/scripts/build-virtual-input.sh" 2>/dev/null | tail -1
}

# --- The compositor ---------------------------------------------------------

# sde_test_compositor [extra environment assignments ...]
#   Starts the compositor headless on $SOCK and waits for its socket. Its log is
#   $RUNTIME/comp.log. Extra arguments are environment assignments for this run
#   (e.g. SDE_OUTPUT_SCALE=2).
sde_test_compositor() {
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_OUTPUT_SCALE=1 \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SDE_TEST_SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$SDE_TEST_BUILD/lib" \
        SDE_APP_BIN_DIR="$SDE_TEST_BUILD/bin" \
        "$@" \
        "$SDE_TEST_COMPOSITOR" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
    SDE_TEST_COMPOSITOR_PID=$!
    sde_test_wait_socket "$SOCK" || {
        echo "FAIL: the compositor did not create $XDG_RUNTIME_DIR/$SOCK" >&2
        tail -20 "$RUNTIME/comp.log" >&2 2>/dev/null || true
        exit 1
    }
}

# sde_test_wait_socket <socket> [tenths-of-a-second]
sde_test_wait_socket() {
    local socket="$1" tries="${2:-80}"
    for _ in $(seq 1 "$tries"); do
        [[ -e "$XDG_RUNTIME_DIR/$socket" ]] && return 0
        sleep 0.1
    done
    return 1
}

# sde_test_wait_log <file> <regex> [tenths]: wait for a line to appear (a client
#   that has mapped, say) instead of sleeping a fixed time.
sde_test_wait_log() {
    local file="$1" pattern="$2" tries="${3:-80}"
    for _ in $(seq 1 "$tries"); do
        grep -qE "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

# sde_test_client <command ...>: runs a client in the background (its environment
#   is the caller's) and remembers it, so cleanup kills it whatever happens.
sde_test_client() {
    "$@" &
    SDE_TEST_CLIENT_PIDS+=($!)
    echo "${SDE_TEST_CLIENT_PIDS[-1]}"
}

# sde_test_kill <pid>: stop a client now, and stop tracking it.
sde_test_kill() {
    local pid="$1"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    local kept=() p
    for p in "${SDE_TEST_CLIENT_PIDS[@]}"; do
        [[ "$p" == "$pid" ]] || kept+=("$p")
    done
    SDE_TEST_CLIENT_PIDS=(${kept[@]+"${kept[@]}"})
}

sde_test_cleanup() {
    local pid
    for pid in ${SDE_TEST_CLIENT_PIDS[@]+"${SDE_TEST_CLIENT_PIDS[@]}"}; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    if [[ -n "$SDE_TEST_COMPOSITOR_PID" ]]; then
        # The compositor's own children (the -s shell and what it started) are in
        # its process group; sweep them so nothing outlives the test.
        pkill -P "$SDE_TEST_COMPOSITOR_PID" 2>/dev/null || true
        kill "$SDE_TEST_COMPOSITOR_PID" 2>/dev/null || true
        wait "$SDE_TEST_COMPOSITOR_PID" 2>/dev/null || true
        SDE_TEST_COMPOSITOR_PID=""
    fi
    rm -f "$XDG_RUNTIME_DIR/$SOCK"* 2>/dev/null || true
    [[ -n "${RUNTIME:-}" ]] && rm -rf "$RUNTIME"
}

# --- Assertions -------------------------------------------------------------

# sde_test_check <file> <extended-regex> <description>: the file contains the
#   pattern (a compositor or client log, usually).
sde_test_check() {
    if grep -qE "$2" "$1" 2>/dev/null; then
        echo "  ok: $3"
    else
        echo "  FAIL: $3"
        SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
    fi
}

# sde_test_refute <file> <extended-regex> <description>: the file does *not*
#   contain the pattern.
sde_test_refute() {
    if grep -qE "$2" "$1" 2>/dev/null; then
        echo "  FAIL: $3"
        SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
    else
        echo "  ok: $3"
    fi
}

# sde_test_no_fault [file ...]: none of the logs shows a crash or an assertion.
sde_test_no_fault() {
    local files=("$@")
    [[ ${#files[@]} -eq 0 ]] && files=("$RUNTIME/comp.log")
    if grep -qiE "SIGSEGV|Segmentation fault|VERIFICATION FAILED|ASSERTION FAILED|wl_list_empty" "${files[@]}" 2>/dev/null; then
        echo "  FAIL: a process faulted"
        grep -aiE "SIGSEGV|Segmentation fault|VERIFICATION FAILED|ASSERTION FAILED|wl_list_empty" "${files[@]}" | head -3
        SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
    else
        echo "  ok: no crash or assertion"
    fi
}

# sde_test_finish [file ...]: print the verdict and exit. On failure, show the
#   tail of the compositor log and of any files given, so the CI log has the
#   reason without a rerun.
sde_test_finish() {
    SDE_TEST_FINISHED=1
    if [[ "$SDE_TEST_FAILURES" -ne 0 ]]; then
        echo "--- compositor log (tail) ---"
        tail -20 "$RUNTIME/comp.log" 2>/dev/null || true
        local file
        for file in "$@"; do
            echo "--- $file (tail) ---"
            tail -20 "$file" 2>/dev/null || true
        done
        echo "==> $SDE_TEST_NAME test FAILED ($SDE_TEST_FAILURES check(s))"
        exit 1
    fi
    echo "==> $SDE_TEST_NAME test PASSED"
}
