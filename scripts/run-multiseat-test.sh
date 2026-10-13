#!/usr/bin/env bash
# Headless test for R4.3/R6 multi-seat & VT-switch polish:
#   (A) seat1: per-seat Wayland socket, supervisor pid file, /bin shadow dir and
#       WindowManagerServer ("WM portal") path;
#   (B) seat0 keeps the bare runtime names;
#   (C) XDG_VTNR is propagated to the session environment;
#   (D) the native preflight gives guidance (and fails) with no DRM device;
#   (E) a client honours SDE_WM_PORTAL_PATH, so a relocated WM portal works
#       (the port's LibCore::SessionManagement relocation; libgui-wayland 0041).
#
# (A)-(D) use stub commands and need no compositor; (E) needs the headless
# compositor + the Taskbar and is skipped if they are not built.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
SDE="$SDE_ROOT/scripts/sde-session.sh"
START="$SDE_ROOT/scripts/start-sde-session.sh"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-multiseat.XXXXXX")"
export XDG_RUNTIME_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
# The supervisor's /bin-shadow helpers land in a (seat-namespaced) runtime dir.
trap 'rm -rf "$RUNTIME"; rm -f "$RUNTIME"/sde-session*.pid 2>/dev/null || true' EXIT

fail=0
check() { if grep -qF "$1" <<<"$2"; then echo "  ok: $1"; else echo "  MISSING ($2): $1"; fail=1; fi; return 0; }
check_absent() { if grep -qE "$1" <<<"$2"; then echo "  UNEXPECTED ($2): $1"; fail=1; else echo "  ok (absent): $1"; fi; return 0; }

# A stub compositor: record the session env we care about, then exit cleanly.
cat > "$RUNTIME/stub-comp" <<'EOF'
#!/bin/sh
env | grep -E '^(SDE_SESSION_DISPLAY|XDG_SEAT|XDG_VTNR|SDE_WM_PORTAL_PATH|SDE_SESSION_PID_FILE|SDE_SESSION_BIN_DIR)=' | sort > "$SDE_TEST_ENV_FILE"
sleep 0.3
EOF
chmod +x "$RUNTIME/stub-comp"

run_session() {
    # $1 = env file, $2 = seat (empty = let it default), $3 = vt (optional)
    local env_file="$1" seat="$2" vt="${3:-}"
    env -u XDG_SEAT -u XDG_VTNR -u SDE_WM_PORTAL_PATH \
        ${seat:+XDG_SEAT="$seat"} ${vt:+XDG_VTNR="$vt"} \
        SDE_TEST_ENV_FILE="$env_file" \
        SDE_SESSION_COMPOSITOR="$RUNTIME/stub-comp" \
        SDE_SESSION_AUTOSTART=0 SDE_SESSION_MAX_RESTARTS=0 SDE_SESSION_RESTART_DELAY=0 \
        "$SDE"
}

echo "== (A) seat1 namespaces the runtime, and exports seat identity =="
run_session "$RUNTIME/env-a" seat1 7 > "$RUNTIME/a.log" 2>&1 || true
cat "$RUNTIME/a.log"
env_a="$(cat "$RUNTIME/env-a" 2>/dev/null || true)"
check "seat 'seat1'" "$(cat "$RUNTIME/a.log")"
check "vt7" "$(cat "$RUNTIME/a.log")"
check "SDE_SESSION_DISPLAY=sde-seat1" "$env_a"
check "XDG_SEAT=seat1" "$env_a"
check "XDG_VTNR=7" "$env_a"
check "SDE_WM_PORTAL_PATH=/tmp/portal/wm-seat1" "$env_a"
check "SDE_SESSION_PID_FILE=$RUNTIME/sde-session-seat1.pid" "$env_a"
check "SDE_SESSION_BIN_DIR=$RUNTIME/sde-session-bin-seat1" "$env_a"

echo "== (B) seat0 keeps the bare runtime names =="
run_session "$RUNTIME/env-b" "" > "$RUNTIME/b.log" 2>&1 || true
cat "$RUNTIME/b.log"
env_b="$(cat "$RUNTIME/env-b" 2>/dev/null || true)"
check "seat 'seat0'" "$(cat "$RUNTIME/b.log")"
check "SDE_SESSION_DISPLAY=sde" "$env_b"
check "XDG_SEAT=seat0" "$env_b"
check "SDE_SESSION_PID_FILE=$RUNTIME/sde-session.pid" "$env_b"
check "SDE_SESSION_BIN_DIR=$RUNTIME/sde-session-bin" "$env_b"
check_absent "^SDE_WM_PORTAL_PATH=" "$env_b"

echo "== (C) XDG_VTNR is inherited when already set by the display manager =="
run_session "$RUNTIME/env-c" seat0 3 > "$RUNTIME/c.log" 2>&1 || true
check "XDG_VTNR=3" "$(cat "$RUNTIME/env-c" 2>/dev/null || true)"

echo "== (D) native preflight refuses without a DRM device =="
# start-sde-session.sh logs to SDE_SESSION_LOG when it has no terminal, so point it
# at the file we assert on.
if env SDE_SESSION_LOG="$RUNTIME/d.log" SDE_DRM_GLOB="/nonexistent/card*" "$START" >/dev/null 2>&1; then
    echo "  FAIL: started without a DRM device"
    fail=1
else
    echo "  ok: refused without a DRM device"
    check "no DRM device" "$(cat "$RUNTIME/d.log")"
fi

echo "== (E) clients honour SDE_WM_PORTAL_PATH (relocated WM portal) =="
if [[ -x "$COMP" && -x "$BUILD/bin/Taskbar" ]]; then
    "$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
    sock="sde-multiseat-$$"
    portal="$RUNTIME/portal-wm"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        SDE_WM_PORTAL_PATH="$portal" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$RUNTIME/comp.log" 2>&1 &
    comp_pid=$!
    for _ in $(seq 1 80); do
        [[ -e "$RUNTIME/$sock" ]] && break
        sleep 0.1
    done
    if [[ -S "$portal" ]]; then
        echo "  ok: compositor listens on the relocated WM portal"
    else
        echo "  MISSING: compositor did not listen on $portal"; fail=1
    fi
    env WAYLAND_DISPLAY="$sock" SDE_WM_PORTAL_PATH="$portal" \
        SDE_APP_BIN_DIR="$BUILD/bin" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        timeout 4 "$BUILD/bin/Taskbar" > "$RUNTIME/taskbar.log" 2>&1 || true
    # The Taskbar registers its applet area with the WM server over the portal; the
    # compositor logs it. (Without the relocation, it would not reach us.)
    if grep -q "AppletManager: applet area size" "$RUNTIME/comp.log"; then
        echo "  ok: the Taskbar reached the relocated WM portal"
    else
        echo "  MISSING: the Taskbar did not reach the relocated WM portal"; fail=1
    fi
    kill "$comp_pid" 2>/dev/null || true
    wait "$comp_pid" 2>/dev/null || true
else
    echo "  SKIP: $COMP or $BUILD/bin/Taskbar not built"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> multiseat test FAILED"
    exit 1
fi
echo "==> multiseat test PASSED"
