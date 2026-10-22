#!/usr/bin/env bash
# ci-tier: full
# Headless test for the installed package layout (R6 packaging).
#
# scripts/install-sde.sh stages a prefix with bin/, lib/, share/res/, the runtime
# scripts and a launcher; the launcher derives its own prefix, so a staged tree
# can be run without being copied to /usr. This test stages one under a temp
# directory, asserts the layout, starts a headless session from the installed
# launcher, waits for the Taskbar, and stops it with SIGTERM.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TASKBAR="$BUILD/bin/Taskbar"

if [[ ! -x "$COMP" || ! -x "$TASKBAR" ]]; then
    echo "SKIP: the SDE or the Taskbar is not built"
    exit 0
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-install.XXXXXX")"
chmod 700 "$RUNTIME"
ROOT="$RUNTIME/root"
PREFIX="$ROOT/usr/lib/sde"
LAUNCHER="$PREFIX/bin/sde-session"
SESSION_LOG="$RUNTIME/session.log"
SESSION_PID=""
cleanup() {
    if [[ -n "$SESSION_PID" ]]; then
        kill -TERM -- "-$SESSION_PID" 2>/dev/null || kill -TERM "$SESSION_PID" 2>/dev/null || true
        wait "$SESSION_PID" 2>/dev/null || true
    fi
    pkill -f "WindowServer -n sde-install-$$" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

mkdir -p "$RUNTIME/run" "$RUNTIME/state" "$RUNTIME/home" "$RUNTIME/session"
chmod 700 "$RUNTIME/run"
# LaunchServer adopts this rather than waiting for Xwayland; the test runs with
# Xwayland off so there is no compositor-written file.
echo ':99' > "$RUNTIME/state/xwayland-display"

echo "=== install the prefix ==="
SDE_ROOT="$SDE_ROOT" BUILD="$BUILD" SERENITY="$SERENITY" \
    "$SDE_ROOT/scripts/install-sde.sh" --destdir "$ROOT" --prefix /usr/lib/sde \
    > "$RUNTIME/install.log" 2>&1 || {
    echo "  FAIL: install failed"
    tail -30 "$RUNTIME/install.log"
    exit 1
}
cat "$RUNTIME/install.log"

fail=0
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}

echo "=== layout ==="
check "the compositor is installed" "[[ -x '$PREFIX/bin/WindowServer' ]]"
check "the Taskbar is installed" "[[ -x '$PREFIX/bin/Taskbar' ]]"
check "LaunchServer is installed" "[[ -x '$PREFIX/bin/LaunchServer' ]]"
check "sde-idle is installed" "[[ -x '$PREFIX/bin/sde-idle' ]]"
check "framework libraries are installed" "[[ -n \"\$(find '$PREFIX/lib' -maxdepth 1 -name 'liblagom-*.so*' -print -quit)\" ]]"
check "resources are installed" "[[ -d '$PREFIX/share/res/themes' && -d '$PREFIX/share/res/fonts' ]]"
check "the runtime scripts are installed" "[[ -x '$PREFIX/share/sde/scripts/start-sde-session.sh' ]]"
check "the applet supervisor is installed" "[[ -x '$PREFIX/share/sde/scripts/sde-supervise-applet.sh' ]]"
check "the polkit agent is installed" "[[ -x '$PREFIX/share/sde/scripts/sde-polkit-agent.py' && -x '$PREFIX/bin/sde-auth-dialog' ]]"
check "the launcher is installed" "[[ -x '$LAUNCHER' ]]"
check "the session entry is installed" "[[ -f '$ROOT/usr/share/wayland-sessions/sde.desktop' ]]"
check "the session entry runs the installed launcher" \
    "grep -qF 'Exec=/usr/lib/sde/bin/sde-session' '$ROOT/usr/share/wayland-sessions/sde.desktop'"

echo "=== a headless session from the installed launcher ==="
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    XDG_RUNTIME_DIR="$RUNTIME/run" \
    HOME="$RUNTIME/home" \
    SDE_STATE_DIR="$RUNTIME/state" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_SESSION_LOG="$SESSION_LOG" \
    SDE_SESSION_PID_FILE="$RUNTIME/session.pid" \
    SDE_SESSION_DISPLAY="sde-install-$$" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SDE_BACKENDS=headless SDE_ALLOW_NO_DRM=1 SDE_XWAYLAND=0 \
    SDE_SESSION_PORTAL=0 \
    SDE_SESSION_IDLE_SECONDS=0 SDE_SESSION_IDLE_DPMS_SECONDS=0 \
    SDE_SESSION_RESTORE=0 SDE_SESSION_AUTOSTART=0 \
    "$LAUNCHER" > "$RUNTIME/launcher.log" 2>&1 &
SESSION_PID=$!

for _ in $(seq 1 200); do
    grep -qF "layer surface namespace='serenity-taskbar'" "$SESSION_LOG" 2>/dev/null && break
    kill -0 "$SESSION_PID" 2>/dev/null || break
    sleep 0.1
done
# LaunchServer adopts the Xwayland display (or waits 5 s for the compositor to
# write one) before it listens; the Taskbar can map before it is ready.
for _ in $(seq 1 150); do
    grep -qF 'SDE LaunchServer: listening' "$SESSION_LOG" 2>/dev/null && break
    sleep 0.1
done

check "the installed session is still running" "kill -0 '$SESSION_PID' 2>/dev/null"
check "the installed Taskbar mapped" \
    "grep -qF \"layer surface namespace='serenity-taskbar'\" '$SESSION_LOG' 2>/dev/null"
check "the installed LaunchServer started on the private session" \
    "grep -qF \"SDE LaunchServer: listening on '$RUNTIME/session/portal/launch'\" '$SESSION_LOG' 2>/dev/null"

kill -TERM -- "-$SESSION_PID" 2>/dev/null || kill -TERM "$SESSION_PID" 2>/dev/null || true
wait "$SESSION_PID" 2>/dev/null || true
SESSION_PID=""
sleep 0.3
check "the session stopped on SIGTERM" \
    "! pgrep -f 'WindowServer -n sde-install-$$' >/dev/null 2>&1"

if [[ "$fail" -ne 0 ]]; then
    echo "--- session log (tail) ---"
    tail -40 "$SESSION_LOG" 2>/dev/null || true
    echo "--- launcher log (tail) ---"
    tail -40 "$RUNTIME/launcher.log" 2>/dev/null || true
    echo "==> install-layout test FAILED"
    exit 1
fi
echo "==> install-layout test PASSED"
