#!/usr/bin/env bash
# sde-sleep.sh - run suspend/resume hooks from logind's PrepareForSleep signal
# (R4.3/R6). Sleeping is a good moment to lock the session, and waking is a good
# moment to unblank the screen and re-apply state; the compositor itself relies on
# libseat/wlroots to release and re-acquire DRM master.
#
# Portable: it watches the system D-Bus with gdbus or dbus-monitor when available
# and does nothing when neither (or logind) is present, so it never blocks a
# session. Set SDE_SLEEP_MONITOR_CMD to a command that prints
# "PrepareForSleep(true)" / "PrepareForSleep(false)" lines (used by tests).
#
# Env: SDE_SUSPEND_LOCK_ON  1 to lock on suspend (default 1)
#      SDE_LOCK_CMD         lock client (default swaylock)
#      SDE_DPMS_BIN         sde-dpms path (default build/tools/sde-dpms)
#      SDE_RESUME_CMD       command run on resume (default "$SDE_DPMS_BIN on")
#      SDE_SUSPEND_HOOK     extra command run on suspend
#      SDE_RESUME_HOOK      extra command run on resume
set -u

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOCK_ON_SUSPEND="${SDE_SUSPEND_LOCK_ON:-1}"
LOCK_CMD="${SDE_LOCK_CMD:-swaylock}"
DPMS_BIN="${SDE_DPMS_BIN:-$SDE_ROOT/build/tools/sde-dpms}"
RESUME_CMD="${SDE_RESUME_CMD:-$DPMS_BIN on}"
SUSPEND_HOOK="${SDE_SUSPEND_HOOK:-}"
RESUME_HOOK="${SDE_RESUME_HOOK:-}"

run() {
    echo "sde-sleep: running: $*"
    setsid sh -c "$*" >/dev/null 2>&1 || true
}

on_suspend() {
    echo "sde-sleep: suspending"
    if [ "$LOCK_ON_SUSPEND" = "1" ] && [ -n "$LOCK_CMD" ]; then
        run "$LOCK_CMD"
    fi
    [ -n "$SUSPEND_HOOK" ] && run "$SUSPEND_HOOK"
}

on_resume() {
    echo "sde-sleep: resumed"
    [ -n "$RESUME_CMD" ] && run "$RESUME_CMD"
    [ -n "$RESUME_HOOK" ] && run "$RESUME_HOOK"
}

monitor_dbus() {
    if [ -n "${SDE_SLEEP_MONITOR_CMD:-}" ]; then
        sh -c "$SDE_SLEEP_MONITOR_CMD"
    elif command -v gdbus >/dev/null 2>&1; then
        gdbus monitor --system --dest org.freedesktop.login1 --object-path /org/freedesktop/login1 2>/dev/null
    elif command -v dbus-monitor >/dev/null 2>&1; then
        dbus-monitor --system "type='signal',interface='org.freedesktop.login1.Manager',member='PrepareForSleep'" 2>/dev/null
    else
        return 1
    fi
}

if ! monitor_dbus | while IFS= read -r line; do
    case "$line" in
        *'PrepareForSleep(true)'* | *'boolean true'*) on_suspend ;;
        *'PrepareForSleep(false)'* | *'boolean false'*) on_resume ;;
    esac
done; then
    # No monitor: nothing to watch (no logind / no D-Bus). Sleep hooks are disabled.
    [ -z "${SDE_SLEEP_MONITOR_CMD:-}" ] && echo "sde-sleep: no D-Bus monitor; suspend/hooks disabled" >&2
    exit 0
fi
