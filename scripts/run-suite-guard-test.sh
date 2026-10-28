#!/usr/bin/env bash
# ci-tier: fast
# The suite driver's live-session guard.
#
# run-all-tests.sh must refuse to run when a live session is visible, and it must
# not be fooled by a container: a container cannot see the host's /run, so it
# refuses unless the caller mounts that runtime, names the host heartbeat, or
# asserts SDE_NO_LIVE_SESSION=1 (CI does). --check-guard runs exactly the guard
# and exits, so this test needs no compositor or test payload.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DRIVER="$SDE_ROOT/scripts/run-all-tests.sh"
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-guard.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

fail=0
pass() { echo "  ok: $1"; }
bad() { echo "  FAIL: $1"; fail=1; }

make_socket() {
    python3 - "$1" <<'PY'
import socket
import sys
s = socket.socket(socket.AF_UNIX)
s.bind(sys.argv[1])
s.close()
PY
}

# check_guard <expected-exit> <name> <output-pattern-or-empty> [VAR=value ...]
check_guard() {
    local expected="$1" name="$2" pattern="$3"
    shift 3
    local output status
    output="$(env -u SDE_LIVE_SESSION -u SDE_HOST_RUNTIME_DIR \
        -u SDE_HOST_HEARTBEAT_FILE -u SDE_NO_LIVE_SESSION \
        -u SDE_ALLOW_LIVE_SESSION -u SDE_CONTAINER -u SDE_SESSION_DISPLAY \
        XDG_RUNTIME_DIR="$RUNTIME/empty" "$@" \
        "$DRIVER" --tier fast --check-guard 2>&1)" && status=0 || status=$?
    if [[ "$status" -ne "$expected" ]]; then
        echo "$output" | sed 's/^/      /'
        bad "$name (exit $status, expected $expected)"
        return
    fi
    if [[ -n "$pattern" ]] && ! grep -qF "$pattern" <<<"$output"; then
        echo "$output" | sed 's/^/      /'
        bad "$name (output did not contain '$pattern')"
        return
    fi
    pass "$name"
}

mkdir -p "$RUNTIME/empty" "$RUNTIME/session" "$RUNTIME/host" "$RUNTIME/heartbeat"
make_socket "$RUNTIME/session/sde"
make_socket "$RUNTIME/host/sde"
touch "$RUNTIME/heartbeat/sde-heartbeat-sde"

echo "=== live-session guard ==="
check_guard 0 "an empty runtime is allowed" "running is allowed" \
    SDE_NO_LIVE_SESSION=1
check_guard 3 "a local session socket refuses" "a live SDE session is present" \
    XDG_RUNTIME_DIR="$RUNTIME/session" SDE_NO_LIVE_SESSION=1
check_guard 3 "a mounted host runtime refuses" "a live SDE session is present" \
    SDE_NO_LIVE_SESSION=1 SDE_HOST_RUNTIME_DIR="$RUNTIME/host"
check_guard 3 "a host heartbeat refuses" "a live SDE session is present" \
    SDE_NO_LIVE_SESSION=1 SDE_HOST_HEARTBEAT_FILE="$RUNTIME/heartbeat/sde-heartbeat-sde"
check_guard 3 "an explicit live-session assertion refuses" "a live SDE session is present" \
    SDE_LIVE_SESSION=1 SDE_NO_LIVE_SESSION=1

check_guard 3 "a container without a visibility assertion refuses" \
    "host's SDE session is not visible" \
    SDE_CONTAINER=1
check_guard 0 "a container that asserts no session is allowed" "running is allowed" \
    SDE_CONTAINER=1 SDE_NO_LIVE_SESSION=1
check_guard 0 "a container with a mounted empty runtime is allowed" "running is allowed" \
    SDE_CONTAINER=1 SDE_HOST_RUNTIME_DIR="$RUNTIME/empty"
check_guard 0 "SDE_ALLOW_LIVE_SESSION overrides" "running is allowed" \
    SDE_CONTAINER=1 SDE_LIVE_SESSION=1 SDE_ALLOW_LIVE_SESSION=1

if [[ "$fail" -ne 0 ]]; then
    echo "==> suite-guard test FAILED"
    exit 1
fi
echo "==> suite-guard test PASSED"
