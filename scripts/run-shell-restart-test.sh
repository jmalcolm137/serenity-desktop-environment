#!/usr/bin/env bash
# Headless test: the shell wrapper keeps the Taskbar running.
#
# A Taskbar that survives startup is the session's shell; if it exits later the
# wrapper must restart it (bounded), not leave the session without a panel. A
# Taskbar that cannot start at all falls back to a Terminal, and a wrapper that
# is stopped (logout, compositor restart) must not restart anything.
#
# Runs with shell scripts only: no compositor, no display.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
WRAPPER="$SDE_ROOT/scripts/sde-shell.sh"
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-shell-restart.XXXXXX")"
chmod 700 "$RUNTIME"

WRAPPER_PID=""
cleanup() {
    if [[ -n "$WRAPPER_PID" ]]; then
        kill "$WRAPPER_PID" 2>/dev/null || true
        wait "$WRAPPER_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

cat > "$RUNTIME/taskbar" <<'EOF'
#!/bin/sh
echo "$$" >> "${SDE_TEST_TASKBAR_LOG:?}"
exec sleep 1000
EOF

cat > "$RUNTIME/failing-taskbar" <<'EOF'
#!/bin/sh
echo "$$" >> "${SDE_TEST_TASKBAR_LOG:?}"
exit 1
EOF

cat > "$RUNTIME/terminal" <<'EOF'
#!/bin/sh
echo "started" >> "${SDE_TEST_TERMINAL_LOG:?}"
exec sleep 1000
EOF
chmod +x "$RUNTIME/taskbar" "$RUNTIME/failing-taskbar" "$RUNTIME/terminal"

fail=0

# kill -0 is true for a zombie. GitHub's container init may not reap the
# reparented child before the test checks, so treat Z as stopped.
process_stopped() {
    local pid="$1" state
    if ! kill -0 "$pid" 2>/dev/null; then
        return 0
    fi
    state="$(ps -o stat= -p "$pid" 2>/dev/null || true)"
    [[ "$state" == Z* ]]
}

check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}

wait_for_lines() {
    local file="$1" expected="$2" lines i
    for i in $(seq 1 100); do
        lines=0
        if [[ -f "$file" ]]; then
            lines="$(wc -l < "$file")"
        fi
        if [[ "${lines:-0}" -ge "$expected" ]]; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

run_wrapper() {
    local taskbar="$1" terminal="$2" log="$3"
    shift 3
    env "$@" "$WRAPPER" "$taskbar" "$terminal" > "$log" 2>&1 &
    WRAPPER_PID=$!
}

stop_wrapper() {
    [[ -n "$WRAPPER_PID" ]] || return 0
    kill -TERM "$WRAPPER_PID" 2>/dev/null || true
    wait "$WRAPPER_PID" 2>/dev/null || true
    WRAPPER_PID=""
}

echo "=== a running Taskbar is restarted, and the budget falls back to a Terminal ==="
export SDE_TEST_TASKBAR_LOG="$RUNTIME/taskbar-a.log"
export SDE_TEST_TERMINAL_LOG="$RUNTIME/terminal-a.log"
run_wrapper "$RUNTIME/taskbar" "$RUNTIME/terminal" "$RUNTIME/wrapper-a.log" \
    SDE_SHELL_STARTUP_CHECKS=2 SDE_SHELL_MAX_RESTARTS=2 SDE_SHELL_RESTART_DELAY=1

wait_for_lines "$SDE_TEST_TASKBAR_LOG" 1
wait_for_lines "$RUNTIME/wrapper-a.log" 1
for i in 1 2 3; do
    pid="$(sed -n "${i}p" "$SDE_TEST_TASKBAR_LOG")"
    kill "$pid" 2>/dev/null || true
    if [[ "$i" -lt 3 ]]; then
        wait_for_lines "$SDE_TEST_TASKBAR_LOG" "$((i + 1))"
    else
        wait_for_lines "$SDE_TEST_TERMINAL_LOG" 1
    fi
done
stop_wrapper

check "the Taskbar ran three times (startup and two restarts)" \
    "[[ \"\$(wc -l < '$SDE_TEST_TASKBAR_LOG')\" -eq 3 ]]"
check "the wrapper logged both restarts" \
    "grep -q 'restarting (2/2)' '$RUNTIME/wrapper-a.log'"
check "the third exit fell back to a Terminal" \
    "grep -q 'starting a fallback Terminal' '$RUNTIME/wrapper-a.log' && [[ -s '$SDE_TEST_TERMINAL_LOG' ]]"

echo "=== stopping the wrapper stops the Taskbar and does not restart it ==="
export SDE_TEST_TASKBAR_LOG="$RUNTIME/taskbar-b.log"
export SDE_TEST_TERMINAL_LOG="$RUNTIME/terminal-b.log"
run_wrapper "$RUNTIME/taskbar" "$RUNTIME/terminal" "$RUNTIME/wrapper-b.log" \
    SDE_SHELL_STARTUP_CHECKS=2 SDE_SHELL_MAX_RESTARTS=5 SDE_SHELL_RESTART_DELAY=1
wait_for_lines "$SDE_TEST_TASKBAR_LOG" 1
wait_for_lines "$RUNTIME/wrapper-b.log" 1
taskbar_pid="$(sed -n 1p "$SDE_TEST_TASKBAR_LOG")"
stop_wrapper
for _ in $(seq 1 40); do
    kill -0 "$taskbar_pid" 2>/dev/null || break
    sleep 0.05
done
check "the Taskbar was stopped with the wrapper" "process_stopped '$taskbar_pid'"
check "no restart was scheduled" "! grep -q 'restarting' '$RUNTIME/wrapper-b.log'"
check "no fallback Terminal was started" "[[ ! -s '$SDE_TEST_TERMINAL_LOG' ]]"

echo "=== a Taskbar that cannot start falls back to a Terminal ==="
export SDE_TEST_TASKBAR_LOG="$RUNTIME/taskbar-c.log"
export SDE_TEST_TERMINAL_LOG="$RUNTIME/terminal-c.log"
run_wrapper "$RUNTIME/failing-taskbar" "$RUNTIME/terminal" "$RUNTIME/wrapper-c.log" \
    SDE_SHELL_STARTUP_CHECKS=2 SDE_SHELL_MAX_RESTARTS=1 SDE_SHELL_RESTART_DELAY=1
wait_for_lines "$SDE_TEST_TERMINAL_LOG" 1
stop_wrapper
check "the startup exit was reported" \
    "grep -q 'exited during startup' '$RUNTIME/wrapper-c.log'"
check "the Terminal took over" "[[ -s '$SDE_TEST_TERMINAL_LOG' ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- wrapper log (restart) ---"
    cat "$RUNTIME/wrapper-a.log" 2>/dev/null || true
    echo "==> shell restart test FAILED"
    exit 1
fi
echo "==> shell restart test PASSED"
