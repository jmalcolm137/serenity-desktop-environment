#!/usr/bin/env bash
# Headless test for the R4.3 power provider chain
# (docs/12-r4.3-portable-session-integration.md §3.3): override wins; logind is
# used when present; portable fallbacks when not; "unavailable" reported cleanly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
POWER="$SDE_ROOT/scripts/sde-power.sh"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-power.XXXXXX")"
trap 'rm -rf "$RUNTIME"' EXIT

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
    return 0
}

# Mock loginctl: present iff the first arg is not "list-sessions"; records calls.
cat > "$RUNTIME/loginctl-present" <<'EOF'
#!/bin/sh
if [ "$1" = "list-sessions" ]; then exit 0; fi
echo "$1" >> "$POWER_LOG"
exit 0
EOF
cat > "$RUNTIME/loginctl-absent" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$RUNTIME/loginctl-present" "$RUNTIME/loginctl-absent"

echo "== (A) SDE_POWER_CMD override wins =="
cat > "$RUNTIME/override" <<'EOF'
#!/bin/sh
echo "override:$1" >> "$POWER_LOG"
EOF
chmod +x "$RUNTIME/override"
: > "$RUNTIME/log-a"
env POWER_LOG="$RUNTIME/log-a" SDE_POWER_CMD="$RUNTIME/override" SDE_LOGINCTL_CMD="$RUNTIME/loginctl-present" \
    "$POWER" reboot || true
check "override:reboot" "$RUNTIME/log-a"

echo "== (B) logind used when present =="
: > "$RUNTIME/log-b"
env POWER_LOG="$RUNTIME/log-b" SDE_LOGINCTL_CMD="$RUNTIME/loginctl-present" "$POWER" reboot || true
check "reboot" "$RUNTIME/log-b"
env SDE_LOGINCTL_CMD="$RUNTIME/loginctl-present" "$POWER" --check shutdown | grep -qF available \
    && echo "  ok: --check shutdown = available" || { echo "  FAIL: expected available"; fail=1; }

echo "== (C) portable logout fallback (no logind) stops the supervisor =="
sleep 30 &
VICTIM=$!
echo "$VICTIM" > "$RUNTIME/session.pid"
env SDE_LOGINCTL_CMD="$RUNTIME/loginctl-absent" SDE_SESSION_PID_FILE="$RUNTIME/session.pid" "$POWER" logout || true
sleep 1
if kill -0 "$VICTIM" 2>/dev/null; then
    echo "  FAIL: supervisor not stopped by logout fallback"
    kill -KILL "$VICTIM" 2>/dev/null || true
    fail=1
else
    echo "  ok: logout fallback stopped the supervisor"
fi

echo "== (D) unavailable when nothing can service the action =="
check_out="$(env SDE_LOGINCTL_CMD="$RUNTIME/loginctl-absent" "$POWER" --check shutdown 2>/dev/null || true)"
if [ "$check_out" = "unavailable" ]; then
    echo "  ok: --check shutdown = unavailable"
else
    echo "  FAIL: expected unavailable, got '${check_out}'"
    fail=1
fi
set +e
env SDE_LOGINCTL_CMD="$RUNTIME/loginctl-absent" "$POWER" shutdown 2>/dev/null
rc=$?
set -e
if [ "$rc" -eq 3 ]; then
    echo "  ok: shutdown exits 3 (unavailable)"
else
    echo "  FAIL: expected exit 3, got $rc"
    fail=1
fi

echo "== (E) invalid action rejected =="
if "$POWER" bogus >/dev/null 2>&1; then
    echo "  FAIL: bogus accepted"
    fail=1
else
    echo "  ok: bogus rejected"
fi

if [ "$fail" -ne 0 ]; then
    echo "==> power providers test FAILED"
    exit 1
fi
echo "==> power providers test PASSED"
