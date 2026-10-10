#!/usr/bin/env bash
# Headless session test (R4.2, docs/11-r4-implementation-plan.md):
#   (A) XDG Autostart entries run (and Hidden=true is skipped);
#   (B) the supervisor restarts a failing compositor, then gives up;
#   (C) power actions route through the portable path (mocked here).
# Uses stub commands, so it needs no compositor or display.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE="$SDE_ROOT/scripts/sde-session.sh"
POWER="$SDE_ROOT/scripts/sde-power.sh"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-session.XXXXXX")"
chmod 700 "$RUNTIME"
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
check_file() {
    if [ -e "$1" ]; then
        echo "  ok: $1 exists"
    else
        echo "  MISSING: $1"
        fail=1
    fi
    return 0
}
check_absent_file() {
    if [ -e "$1" ]; then
        echo "  UNEXPECTED: $1 exists"
        fail=1
    else
        echo "  ok (absent): $1"
    fi
    return 0
}

echo "== (A) XDG Autostart runs; Hidden=true is skipped =="
mkdir -p "$RUNTIME/autostart"
cat > "$RUNTIME/autostart/run.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Runs
Exec=touch $RUNTIME/autostarted
EOF
cat > "$RUNTIME/autostart/skip.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Skip
Hidden=true
Exec=touch $RUNTIME/should-not-run
EOF

env SDE_SESSION_COMPOSITOR="sleep 0.5" \
    SDE_SESSION_AUTOSTART_DIR="$RUNTIME/autostart" \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-a.log" 2>&1 || true
cat "$RUNTIME/session-a.log"
check "autostart run.desktop" "$RUNTIME/session-a.log"
check_file "$RUNTIME/autostarted"
check_absent_file "$RUNTIME/should-not-run"

echo "== (B) supervisor restarts a failing compositor, then gives up =="
env SDE_SESSION_COMPOSITOR="exit 1" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_MAX_RESTARTS=2 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-b.log" 2>&1 || true
cat "$RUNTIME/session-b.log"
check "restart 1/2" "$RUNTIME/session-b.log"
check "restart 2/2" "$RUNTIME/session-b.log"
check "giving up" "$RUNTIME/session-b.log"

echo "== (C) power routes through the portable path (mock) =="
cat > "$RUNTIME/power-mock" <<'EOF'
#!/bin/sh
echo "$1" >> "$POWER_LOG"
EOF
chmod +x "$RUNTIME/power-mock"
for action in shutdown reboot logout suspend; do
    env SDE_POWER_CMD="$RUNTIME/power-mock" POWER_LOG="$RUNTIME/power.log" "$POWER" "$action" || true
done
cat "$RUNTIME/power.log" 2>/dev/null || true
for action in shutdown reboot logout suspend; do
    check "$action" "$RUNTIME/power.log"
done
if env SDE_POWER_CMD="$RUNTIME/power-mock" "$POWER" bogus >/dev/null 2>&1; then
    echo "  UNEXPECTED: bogus action accepted"
    fail=1
else
    echo "  ok: bogus action rejected"
fi

if [ "$fail" -ne 0 ]; then
    echo "==> session test FAILED"
    exit 1
fi
echo "==> session test PASSED"
