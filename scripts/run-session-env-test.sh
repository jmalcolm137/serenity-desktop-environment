#!/usr/bin/env bash
# Headless test for the R4.3 session environment
# (docs/12-r4.3-portable-session-integration.md §3.1): the supervisor sets the
# standard session variables and best-effort pushes them to the D-Bus activation
# environment, without requiring systemd or the helper.
set -euo pipefail

# The session updater must not create directories in the developer home.
export SDE_USER_DIRS_UPDATE=0

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SDE="$SDE_ROOT/scripts/sde-session.sh"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-env.XXXXXX")"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT

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

# A stub "compositor" that records the environment the supervisor gave it and
# creates the session socket (so autostart isn't delayed waiting for it).
cat > "$RUNTIME/compositor" <<'EOF'
#!/bin/sh
env > "$ENV_OUT"
: > "${XDG_RUNTIME_DIR:-/tmp}/sde"
exit 0
EOF
chmod +x "$RUNTIME/compositor"

# An autostart entry that records the *session* environment (what an app sees).
mkdir -p "$RUNTIME/autostart"
cat > "$RUNTIME/autostart/env.desktop" <<EOF
[Desktop Entry]
Type=Application
Exec=sh -c 'env > $RUNTIME/session-env.txt'
EOF

echo "== (A) session environment is set =="
env -u WAYLAND_DISPLAY -u DBUS_SESSION_BUS_ADDRESS \
    ENV_OUT="$RUNTIME/env.txt" \
    SDE_SESSION_COMPOSITOR="$RUNTIME/compositor" \
    SDE_SESSION_AUTOSTART=1 \
    SDE_SESSION_AUTOSTART_DIR="$RUNTIME/autostart" \
    SDE_SESSION_PID_FILE="$RUNTIME/session.pid" \
    XDG_RUNTIME_DIR="$RUNTIME" \
    "$SDE" > "$RUNTIME/session.log" 2>&1 || true
for _ in $(seq 1 50); do [ -s "$RUNTIME/session-env.txt" ] && break; sleep 0.1; done
cat "$RUNTIME/env.txt" 2>/dev/null | grep -E "XDG_SESSION|DESKTOP_SESSION|WAYLAND_DISPLAY|SDE_SESSION_PID_FILE" || true
# The compositor sees the standard session identity...
check "XDG_SESSION_TYPE=wayland" "$RUNTIME/env.txt"
check "XDG_SESSION_CLASS=user" "$RUNTIME/env.txt"
check "XDG_CURRENT_DESKTOP=SDE" "$RUNTIME/env.txt"
check "XDG_SESSION_DESKTOP=SDE" "$RUNTIME/env.txt"
check "DESKTOP_SESSION=sde" "$RUNTIME/env.txt"
# ...but must NOT inherit the session socket, or a native session selects the
# nested Wayland backend instead of DRM (see docs/12 and sde-session.sh).
if grep -qF "WAYLAND_DISPLAY=sde" "$RUNTIME/env.txt"; then
    echo "  FAIL: compositor inherited WAYLAND_DISPLAY=sde (should go native)"
    fail=1
else
    echo "  ok: compositor did not inherit the session display"
fi
# The session (apps/autostart) targets the session socket.
check "WAYLAND_DISPLAY=sde" "$RUNTIME/session-env.txt"
check "XDG_CURRENT_DESKTOP=SDE" "$RUNTIME/session-env.txt"

if [ -e "$RUNTIME/session.pid" ]; then
    echo "  FAIL: pid file not cleaned up"
    fail=1
else
    echo "  ok: pid file cleaned up on exit"
fi

echo "== (B) D-Bus activation environment is pushed (best effort) =="
cat > "$RUNTIME/dbus-env" <<'EOF'
#!/bin/sh
echo "$@" > "$DBUS_LOG_1"
EOF
chmod +x "$RUNTIME/dbus-env"
env -u WAYLAND_DISPLAY \
    ENV_OUT="$RUNTIME/env2.txt" \
    SDE_SESSION_COMPOSITOR="$RUNTIME/compositor" \
    SDE_SESSION_AUTOSTART=0 \
    SDE_SESSION_PID_FILE="$RUNTIME/session2.pid" \
    SDE_DBUS_ENV_CMD="$RUNTIME/dbus-env" \
    DBUS_LOG_1="$RUNTIME/dbus.log" \
    XDG_RUNTIME_DIR="$RUNTIME" \
    "$SDE" > "$RUNTIME/session2.log" 2>&1 || true
check "XDG_SESSION_TYPE" "$RUNTIME/dbus.log"
check "WAYLAND_DISPLAY" "$RUNTIME/dbus.log"

echo "== (C) missing D-Bus helper does not fail the session =="
if grep -qF "SDE session: starting compositor" "$RUNTIME/session2.log"; then
    echo "  ok: supervisor still started the compositor without a dbus helper"
else
    echo "  FAIL: supervisor did not start"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "==> session env test FAILED"
    exit 1
fi
echo "==> session env test PASSED"
