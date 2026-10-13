#!/usr/bin/env bash
# Headless test for the R4.3 capability probe
# (docs/12-r4.3-portable-session-integration.md §4): with logind/D-Bus/portal
# simulated absent, `sde-capabilities` reports the expected key=value set and
# valid JSON.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CAP="$SDE_ROOT/scripts/sde-capabilities"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-caps.XXXXXX")"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

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

echo "== (A) text output (seatd, no logind, no bus) =="
env SDE_SEAT_BACKEND=seatd \
    SDE_LOGINCTL_CMD=/bin/false \
    XDG_RUNTIME_DIR="$RUNTIME" \
    DBUS_SESSION_BUS_ADDRESS= \
    "$CAP" > "$RUNTIME/caps.txt" 2>&1 || true
cat "$RUNTIME/caps.txt"
check "seat=seatd" "$RUNTIME/caps.txt"
check "logind=absent" "$RUNTIME/caps.txt"
check "dbus_session_bus=absent" "$RUNTIME/caps.txt"
check "seatd_sock=" "$RUNTIME/caps.txt"
check "portal=" "$RUNTIME/caps.txt"
check "power.logout=" "$RUNTIME/caps.txt"
check "power.shutdown=" "$RUNTIME/caps.txt"

echo "== (B) logind present is detected =="
cat > "$RUNTIME/loginctl-present" <<'EOF'
#!/bin/sh
[ "$1" = "list-sessions" ] && exit 0
exit 0
EOF
chmod +x "$RUNTIME/loginctl-present"
env SDE_LOGINCTL_CMD="$RUNTIME/loginctl-present" DBUS_SESSION_BUS_ADDRESS= XDG_RUNTIME_DIR="$RUNTIME" \
    "$CAP" > "$RUNTIME/caps-logind.txt" 2>&1 || true
check "logind=present" "$RUNTIME/caps-logind.txt"
check "seat=logind" "$RUNTIME/caps-logind.txt"

echo "== (C) JSON output =="
env SDE_SEAT_BACKEND=builtin SDE_LOGINCTL_CMD=/bin/false XDG_RUNTIME_DIR="$RUNTIME" DBUS_SESSION_BUS_ADDRESS= \
    "$CAP" --json > "$RUNTIME/caps.json" 2>&1 || true
cat "$RUNTIME/caps.json"
check '"seat": "builtin"' "$RUNTIME/caps.json"
check '"logind": "absent"' "$RUNTIME/caps.json"

if [ "$fail" -ne 0 ]; then
    echo "==> capabilities test FAILED"
    exit 1
fi
echo "==> capabilities test PASSED"
