#!/usr/bin/env bash
# Headless test: a crashed session service is restarted, and a supervised service
# stops with the session.
#
# LaunchServer, ImageDecoder and the portal backend are long-running session
# services. A crash used to stay dead for the rest of the session (docs/07 §4);
# scripts/sde-service-supervisor.sh restarts them with a bounded budget.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUPERVISOR="$SDE_ROOT/scripts/sde-service-supervisor.sh"
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-service-supervisor.XXXXXX")"
chmod 700 "$RUNTIME"

SUPERVISOR_PID=""
cleanup() {
    if [[ -n "$SUPERVISOR_PID" ]]; then
        kill "$SUPERVISOR_PID" 2>/dev/null || true
        wait "$SUPERVISOR_PID" 2>/dev/null || true
    fi
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

cat > "$RUNTIME/service" <<'EOF'
#!/bin/sh
echo "$$" >> "${SDE_TEST_SERVICE_LOG:?}"
if [ "${SDE_TEST_SERVICE_MODE:-run}" = "exit" ]; then
    exit 1
fi
exec sleep 1000
EOF
chmod +x "$RUNTIME/service"

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

echo "=== a crashing service is restarted up to the budget ==="
export SDE_TEST_SERVICE_LOG="$RUNTIME/crash.log"
export SDE_TEST_SERVICE_MODE=exit
SDE_SERVICE_MAX_RESTARTS=2 SDE_SERVICE_RESTART_DELAY=0 \
    "$SUPERVISOR" TestService -- "$RUNTIME/service" > "$RUNTIME/crash-supervisor.log" 2>&1 &
SUPERVISOR_PID=$!
wait "$SUPERVISOR_PID" 2>/dev/null || true
SUPERVISOR_PID=""

check "the service ran three times (startup and two restarts)" \
    "[[ \"\$(wc -l < '$RUNTIME/crash.log')\" -eq 3 ]]"
check "the supervisor logged both restarts" \
    "grep -q 'restarting (2/2)' '$RUNTIME/crash-supervisor.log'"
check "the supervisor gave up after the budget" \
    "grep -q 'exited 3 times; not restarting' '$RUNTIME/crash-supervisor.log'"

echo "=== stopping the supervisor stops the service ==="
export SDE_TEST_SERVICE_LOG="$RUNTIME/run.log"
export SDE_TEST_SERVICE_MODE=run
SDE_SERVICE_MAX_RESTARTS=5 SDE_SERVICE_RESTART_DELAY=0 \
    "$SUPERVISOR" TestService -- "$RUNTIME/service" > "$RUNTIME/run-supervisor.log" 2>&1 &
SUPERVISOR_PID=$!
wait_for_lines "$SDE_TEST_SERVICE_LOG" 1
service_pid="$(sed -n 1p "$SDE_TEST_SERVICE_LOG")"
kill -TERM "$SUPERVISOR_PID" 2>/dev/null || true
wait "$SUPERVISOR_PID" 2>/dev/null || true
SUPERVISOR_PID=""

for _ in $(seq 1 40); do
    kill -0 "$service_pid" 2>/dev/null || break
    sleep 0.05
done
check "the service was stopped with its supervisor" "process_stopped '$service_pid'"
check "no restart was scheduled" "! grep -q 'restarting' '$RUNTIME/run-supervisor.log'"
check "the service ran once" "[[ \"\$(wc -l < '$RUNTIME/run.log')\" -eq 1 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- supervisor log (crash) ---"
    cat "$RUNTIME/crash-supervisor.log" 2>/dev/null || true
    echo "==> service supervisor test FAILED"
    exit 1
fi
echo "==> service supervisor test PASSED"
