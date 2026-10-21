#!/usr/bin/env bash
# Headless session test (R4.2, docs/11-r4-implementation-plan.md):
#   (A) XDG Autostart entries run (and Hidden=true is skipped);
#   (B) the supervisor restarts a failing compositor, then gives up;
#   (C) power actions route through the portable path (mocked here);
#   (D-G) idle-to-lock, /bin power helpers, SIGTERM/SIGHUP, capability probe;
#   (H) the watchdog restarts a wedged compositor (stale heartbeat), (H2) also
#       one that shuts down cleanly (status 0) on the watchdog's SIGTERM;
#   (I) repeated failure drops to safe mode instead of ending the session;
#   (J) manual safe mode starts minimal and skips autostart;
#   (K) session.conf supplies policy and the environment overrides it (R6).
# Uses stub commands, so it needs no compositor or display.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE="$SDE_ROOT/scripts/sde-session.sh"
POWER="$SDE_ROOT/scripts/sde-power.sh"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-session.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT

# Keep the session's /bin-shadow helpers inside the test runtime dir.
export SDE_SESSION_BIN_DIR="$RUNTIME/sbin"

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

echo "== (D) idle-to-lock is armed when configured =="
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
env SDE_SESSION_COMPOSITOR="sleep 1" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_IDLE_SECONDS=300 \
    SDE_SESSION_IDLE_BIN="$SDE_ROOT/build/tools/sde-idle" \
    SDE_LOCK_CMD="sde-lock-test" \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-d.log" 2>&1 || true
cat "$RUNTIME/session-d.log"
check "idle-to-lock after 300s via 'sde-lock-test'" "$RUNTIME/session-d.log"

echo "== (E) /bin power helpers are installed and route =="
env SDE_SESSION_COMPOSITOR="sleep 0.3" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_BIN_DIR="$RUNTIME/sbin" \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-e.log" 2>&1 || true
for helper in shutdown reboot logout; do
    check_file "$RUNTIME/sbin/$helper"
done
env SDE_POWER_CMD="$RUNTIME/power-mock" POWER_LOG="$RUNTIME/power-e.log" \
    "$RUNTIME/sbin/shutdown" --now || true
check "shutdown" "$RUNTIME/power-e.log"

echo "== (F) SIGTERM stops the supervisor (does not restart) =="
env SDE_SESSION_COMPOSITOR="sleep 30" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_MAX_RESTARTS=5 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-f.log" 2>&1 &
SUP_PID=$!
# Like G2, wait for the traps rather than racing a fixed sleep on a slow runner.
for _ in $(seq 1 100); do
    grep -qF 'supervisor ready' "$RUNTIME/session-f.log" 2>/dev/null && break
    sleep 0.1
done
kill -TERM "$SUP_PID" 2>/dev/null || true
# Poll: an exited child can linger as a zombie until reaped, which still
# satisfies kill -0. Treat Z as stopped and reap it with wait below.
stopped=0
for _ in $(seq 1 50); do
    if ! kill -0 "$SUP_PID" 2>/dev/null; then
        stopped=1
        break
    fi
    state="$(ps -o stat= -p "$SUP_PID" 2>/dev/null || true)"
    case "$state" in
        Z*) stopped=1; break ;;
    esac
    sleep 0.1
done
if [ "$stopped" -eq 1 ]; then
    echo "  ok: supervisor stopped on SIGTERM"
    wait "$SUP_PID" 2>/dev/null || true
else
    echo "  FAIL: supervisor still running after SIGTERM"
    kill -KILL "$SUP_PID" 2>/dev/null || true
    wait "$SUP_PID" 2>/dev/null || true
    fail=1
fi
if grep -qF "restart " "$RUNTIME/session-f.log"; then
    echo "  UNEXPECTED: supervisor restarted on SIGTERM"
    fail=1
else
    echo "  ok: no restart on SIGTERM"
fi

echo "== (G2) SIGHUP restarts the compositor in place =="
env SDE_SESSION_COMPOSITOR="sleep 30" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_MAX_RESTARTS=5 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-g2.log" 2>&1 &
SUP_G2=$!
# Wait for the supervisor to install its signal traps; a fixed sleep is a race
# on a slower CI runner.
for _ in $(seq 1 100); do
    grep -qF 'supervisor ready' "$RUNTIME/session-g2.log" 2>/dev/null && break
    sleep 0.1
done
kill -HUP "$SUP_G2" 2>/dev/null || true
for _ in $(seq 1 50); do
    [ "$(grep -c 'starting compositor' "$RUNTIME/session-g2.log" 2>/dev/null || echo 0)" -ge 2 ] && break
    sleep 0.1
done
if kill -0 "$SUP_G2" 2>/dev/null; then
    echo "  ok: supervisor survived SIGHUP"
else
    echo "  FAIL: supervisor exited on SIGHUP"
    fail=1
fi
check "restarting compositor on request" "$RUNTIME/session-g2.log"
if grep -qF "compositor exited; stopping" "$RUNTIME/session-g2.log"; then
    echo "  UNEXPECTED: SIGHUP was treated as a user quit"
    fail=1
else
    echo "  ok: SIGHUP was not treated as a user quit"
fi
kill -TERM "$SUP_G2" 2>/dev/null || true
sleep 0.5
kill -KILL "$SUP_G2" 2>/dev/null || true
wait "$SUP_G2" 2>/dev/null || true

echo "== (G) degraded environment: capability probe =="
mkdir -p "$RUNTIME/caps"
env SDE_SEAT_BACKEND=builtin \
    SDE_LOGINCTL_CMD=/bin/false \
    XDG_RUNTIME_DIR="$RUNTIME/caps" \
    DBUS_SESSION_BUS_ADDRESS= \
    "$SDE_ROOT/scripts/sde-capabilities" > "$RUNTIME/caps.log" 2>&1 || true
cat "$RUNTIME/caps.log"
check "logind=absent" "$RUNTIME/caps.log"
check "dbus_session_bus=absent" "$RUNTIME/caps.log"
check "seat=builtin" "$RUNTIME/caps.log"

echo "== (H) watchdog restarts a wedged compositor (stale heartbeat) =="
cat > "$RUNTIME/hang-comp" <<EOF
#!/bin/sh
echo "\$(date +%s)" > "$RUNTIME/hb"
sleep 30
EOF
chmod +x "$RUNTIME/hang-comp"
env SDE_SESSION_COMPOSITOR="$RUNTIME/hang-comp" \
    SDE_SESSION_HEARTBEAT_FILE="$RUNTIME/hb" \
    SDE_SESSION_WATCHDOG_SECONDS=2 \
    SDE_SESSION_MAX_RESTARTS=5 \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-h.log" 2>&1 &
SUP_H=$!
# The watchdog needs the heartbeat to age past WATCHDOG_SECONDS, then gives the
# compositor 2s to die before SIGKILL, and only then does the main loop restart it.
# A fixed wait is a race against all three; wait for the restart instead.
for _ in $(seq 1 40); do
    grep -q "restart 1/5" "$RUNTIME/session-h.log" 2>/dev/null && break
    sleep 0.5
done
kill -TERM "$SUP_H" 2>/dev/null || true
wait "$SUP_H" 2>/dev/null || true
cat "$RUNTIME/session-h.log"
check "heartbeat stale" "$RUNTIME/session-h.log"
check "restart 1/5" "$RUNTIME/session-h.log"

echo "== (H2) the watchdog restarts a compositor that shuts down cleanly on SIGTERM =="
# The real compositor handles SIGTERM and exits 0, which on its own means "the
# user quit" and stops the session. A stale heartbeat must still restart it.
cat > "$RUNTIME/clean-comp" <<EOF
#!/bin/bash
echo "\$(date +%s)" > "$RUNTIME/hb2"
trap 'exit 0' TERM
sleep 30 &
wait
EOF
chmod +x "$RUNTIME/clean-comp"
env SDE_SESSION_COMPOSITOR="$RUNTIME/clean-comp" \
    SDE_SESSION_HEARTBEAT_FILE="$RUNTIME/hb2" \
    SDE_SESSION_WATCHDOG_SECONDS=2 \
    SDE_SESSION_MAX_RESTARTS=5 \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-h2.log" 2>&1 &
SUP_H2=$!
for _ in $(seq 1 40); do
    grep -q "restart 1/5\|compositor exited; stopping" "$RUNTIME/session-h2.log" 2>/dev/null && break
    sleep 0.5
done
kill -TERM "$SUP_H2" 2>/dev/null || true
wait "$SUP_H2" 2>/dev/null || true
cat "$RUNTIME/session-h2.log"
check "restart 1/5" "$RUNTIME/session-h2.log"

echo "== (H3) a wall-clock jump does not look like a wedge =="
cat > "$RUNTIME/jump-comp" <<EOF
#!/bin/sh
# A heartbeat whose value is deliberately far from wall clock, but which keeps
# changing. The old absolute-time comparison killed this healthy compositor at
# once; the change-counting watchdog must leave it alone.
i=1
echo "\$i" > "$RUNTIME/hb3"
while :; do
    sleep 1
    i=\$((i + 1))
    echo "\$i" > "$RUNTIME/hb3"
done
EOF
chmod +x "$RUNTIME/jump-comp"
env SDE_SESSION_COMPOSITOR="$RUNTIME/jump-comp" \
    SDE_SESSION_HEARTBEAT_FILE="$RUNTIME/hb3" \
    SDE_SESSION_WATCHDOG_SECONDS=2 \
    SDE_SESSION_MAX_RESTARTS=1 \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-h3.log" 2>&1 &
SUP_H3=$!
sleep 5
kill -TERM "$SUP_H3" 2>/dev/null || true
wait "$SUP_H3" 2>/dev/null || true
cat "$RUNTIME/session-h3.log"
if grep -q "heartbeat stale" "$RUNTIME/session-h3.log"; then
    echo "  FAIL: a changing heartbeat was treated as stale"
    fail=1
else
    echo "  ok: a changing heartbeat is not stale"
fi
if grep -q "restart 1/1" "$RUNTIME/session-h3.log"; then
    echo "  FAIL: the healthy compositor was restarted"
    fail=1
else
    echo "  ok: the healthy compositor was not restarted"
fi

echo "== (I) repeated failure drops to safe mode (does not end the session) =="
cat > "$RUNTIME/safe-comp" <<EOF
#!/bin/sh
echo safe >> "$RUNTIME/safe-ran"
exit 1
EOF
chmod +x "$RUNTIME/safe-comp"
env SDE_SESSION_COMPOSITOR="exit 1" \
    SDE_SESSION_COMPOSITOR_SAFE="$RUNTIME/safe-comp" \
    SDE_SESSION_MAX_RESTARTS=1 \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-i.log" 2>&1 || true
cat "$RUNTIME/session-i.log"
check "entering safe mode" "$RUNTIME/session-i.log"
check "SAFE MODE" "$RUNTIME/session-i.log"
check_file "$RUNTIME/safe-ran"

echo "== (J) manual safe mode starts minimal and skips autostart =="
mkdir -p "$RUNTIME/autostart-j"
cat > "$RUNTIME/autostart-j/run.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Runs
Exec=touch $RUNTIME/autostarted-j
EOF
env SDE_SESSION_COMPOSITOR="sleep 0.4" \
    SDE_SESSION_COMPOSITOR_SAFE="sleep 0.4" \
    SDE_SESSION_AUTOSTART=1 \
    SDE_SESSION_AUTOSTART_DIR="$RUNTIME/autostart-j" \
    SDE_SESSION_SAFE_MODE=1 \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-j.log" 2>&1 || true
cat "$RUNTIME/session-j.log"
check "SAFE MODE" "$RUNTIME/session-j.log"
check_absent_file "$RUNTIME/autostarted-j"

echo "== (K) session.conf supplies policy; the environment overrides it =="
mkdir -p "$RUNTIME/conf"
cat > "$RUNTIME/conf/session.conf" <<'EOF'
# SDE session policy
idle_seconds=42
lock_command=sde-lock-conf
max_restarts=9
EOF
env SDE_SESSION_CONF="$RUNTIME/conf/session.conf" \
    SDE_SESSION_COMPOSITOR="sleep 0.4" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_IDLE_BIN="$SDE_ROOT/build/tools/sde-idle" \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-k1.log" 2>&1 || true
cat "$RUNTIME/session-k1.log"
check "loaded session policy" "$RUNTIME/session-k1.log"
check "idle-to-lock after 42s via 'sde-lock-conf'" "$RUNTIME/session-k1.log"
env SDE_SESSION_CONF="$RUNTIME/conf/session.conf" \
    SDE_SESSION_IDLE_SECONDS=7 \
    SDE_SESSION_COMPOSITOR="sleep 0.4" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_IDLE_BIN="$SDE_ROOT/build/tools/sde-idle" \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-k2.log" 2>&1 || true
cat "$RUNTIME/session-k2.log"
check "idle-to-lock after 7s via 'sde-lock-conf'" "$RUNTIME/session-k2.log"

echo "== (L) suspend/resume hooks fire on PrepareForSleep =="
cat > "$RUNTIME/sleep-monitor" <<'EOF'
#!/bin/sh
printf 'PrepareForSleep(true)\n'
sleep 0.1
printf 'PrepareForSleep(false)\n'
EOF
chmod +x "$RUNTIME/sleep-monitor"
env SDE_SLEEP_MONITOR_CMD="$RUNTIME/sleep-monitor" \
    SDE_SUSPEND_LOCK_ON=1 \
    SDE_LOCK_CMD="touch $RUNTIME/locked" \
    SDE_RESUME_CMD= \
    SDE_SUSPEND_HOOK="touch $RUNTIME/suspended" \
    SDE_RESUME_HOOK="touch $RUNTIME/resumed" \
    "$SDE_ROOT/scripts/sde-sleep.sh" > "$RUNTIME/sleep.log" 2>&1 || true
cat "$RUNTIME/sleep.log"
check "sde-sleep: suspending" "$RUNTIME/sleep.log"
check "sde-sleep: resumed" "$RUNTIME/sleep.log"
check_file "$RUNTIME/locked"
check_file "$RUNTIME/suspended"
check_file "$RUNTIME/resumed"

# lock_on_suspend=0 skips the lock but still runs the suspend hook.
env SDE_SLEEP_MONITOR_CMD="$RUNTIME/sleep-monitor" \
    SDE_SUSPEND_LOCK_ON=0 \
    SDE_LOCK_CMD="touch $RUNTIME/locked-2" \
    SDE_RESUME_CMD= \
    SDE_SUSPEND_HOOK="touch $RUNTIME/suspended-2" \
    "$SDE_ROOT/scripts/sde-sleep.sh" >/dev/null 2>&1 || true
check_absent_file "$RUNTIME/locked-2"
check_file "$RUNTIME/suspended-2"

echo "== (M) session restore relaunches the recorded applications =="
# The compositor records the apps that had a window (R4.2); the supervisor
# relaunches them after a (re)start, skipping what its own startup command runs.
mkdir -p "$RUNTIME/bin-k"
cat > "$RUNTIME/bin-k/TextEditor" <<EOF
#!/usr/bin/env bash
touch "$RUNTIME/restored-k"
EOF
cat > "$RUNTIME/bin-k/FileManager" <<EOF
#!/usr/bin/env bash
touch "$RUNTIME/restored-shell-app"
EOF
chmod +x "$RUNTIME/bin-k/TextEditor" "$RUNTIME/bin-k/FileManager"
printf 'TextEditor\nFileManager\nGone\n' > "$RUNTIME/session-apps-k"
env SDE_SESSION_COMPOSITOR="sleep 0.5" \
    SDE_SESSION_APP_BIN_DIR="$RUNTIME/bin-k" \
    SDE_SESSION_RESTORE_FILE="$RUNTIME/session-apps-k" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_SHELL="$RUNTIME/bin-k/FileManager -d" \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-k.log" 2>&1 || true
check "restoring TextEditor" "$RUNTIME/session-k.log"
check "'Gone' is no longer available" "$RUNTIME/session-k.log"
check_file "$RUNTIME/restored-k"
# The shell's own app is not relaunched a second time.
check_absent_file "$RUNTIME/restored-shell-app"

# The shell wrapper's fallback Terminal is not an unconditional startup app: a
# Terminal that had a window must be restored.
mkdir -p "$RUNTIME/bin-l"
cat > "$RUNTIME/bin-l/Terminal" <<EOF
#!/usr/bin/env bash
touch "$RUNTIME/restored-terminal-l"
EOF
cat > "$RUNTIME/bin-l/FileManager" <<EOF
#!/usr/bin/env bash
touch "$RUNTIME/restored-shell-app-l"
EOF
chmod +x "$RUNTIME/bin-l/Terminal" "$RUNTIME/bin-l/FileManager"
printf 'Terminal\nFileManager\n' > "$RUNTIME/session-apps-l"
env SDE_SESSION_COMPOSITOR="sleep 0.5" \
    SDE_SESSION_APP_BIN_DIR="$RUNTIME/bin-l" \
    SDE_SESSION_RESTORE_FILE="$RUNTIME/session-apps-l" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_SHELL="$RUNTIME/bin-l/sde-shell.sh $RUNTIME/bin-l/FileManager $RUNTIME/bin-l/Terminal" \
    SDE_SESSION_STARTUP_APPS="FileManager" \
    SDE_SESSION_RESTART_DELAY=0 \
    "$SDE" > "$RUNTIME/session-l.log" 2>&1 || true
check "restoring Terminal" "$RUNTIME/session-l.log"
check_file "$RUNTIME/restored-terminal-l"
check_absent_file "$RUNTIME/restored-shell-app-l"

if [ "$fail" -ne 0 ]; then
    echo "==> session test FAILED"
    exit 1
fi
echo "==> session test PASSED"
