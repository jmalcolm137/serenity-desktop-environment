#!/usr/bin/env bash
# SDE session power helper (R4.2/R4.3, docs/12-r4.3-portable-session-integration.md).
#
# Turns power into an *ordered provider chain* so the session works with or
# without logind/systemd:
#
#   1. SDE_POWER_CMD override (test/embed seam)
#   2. logind (org.freedesktop.login1 via loginctl, else busctl)
#   3. portable fallbacks (logout -> stop the supervisor; suspend -> rtcwake/pm-suspend)
#   4. unavailable (clear message)
#
# Usage:
#   sde-power.sh <logout|reboot|shutdown|suspend>     perform the action
#   sde-power.sh --check <action>                     print available|unavailable
#   sde-power.sh --list                               print every action's availability
#
# Seams: SDE_POWER_CMD, SDE_LOGINCTL_CMD, SDE_BUSCTL_CMD, SDE_SESSION_PID_FILE,
# SDE_SUSPEND_CMD.
set -eu

VALID_ACTIONS="logout reboot shutdown suspend"
is_valid_action() {
    case " $VALID_ACTIONS " in *" $1 "*) return 0 ;; *) return 1 ;; esac
}

logind_present() {
    local loginctl="${SDE_LOGINCTL_CMD:-loginctl}"
    command -v "${loginctl%% *}" >/dev/null 2>&1 || return 1
    "$loginctl" list-sessions >/dev/null 2>&1
}

# --- provider: fallbacks ---------------------------------------------------
fallback_present() {
    case "$1" in
        logout)
            [ -n "${SDE_SESSION_PID_FILE:-}" ] && [ -f "${SDE_SESSION_PID_FILE}" ]
            ;;
        suspend)
            command -v "${SDE_SUSPEND_CMD:-rtcwake}" >/dev/null 2>&1 || command -v pm-suspend >/dev/null 2>&1
            ;;
        *)
            return 1
            ;;
    esac
}

availability() {
    local action="$1"
    if [ -n "${SDE_POWER_CMD:-}" ]; then
        echo available
        return 0
    fi
    if logind_present; then
        echo available
        return 0
    fi
    if fallback_present "$action"; then
        echo available
        return 0
    fi
    echo unavailable
    return 1
}

do_logind() {
    local action="$1"
    local loginctl="${SDE_LOGINCTL_CMD:-loginctl}"
    if command -v "${loginctl%% *}" >/dev/null 2>&1; then
        case "$action" in
            logout)
                local session="${XDG_SESSION_ID:-$("$loginctl" --no-legend 2>/dev/null | awk -v u="$USER" '$3==u {print $1; exit}')}"
                [ -n "$session" ] || { echo "sde-power: no session to log out" >&2; return 1; }
                exec "$loginctl" terminate-session "$session"
                ;;
            reboot) exec "$loginctl" reboot ;;
            shutdown) exec "$loginctl" poweroff ;;
            suspend) exec "$loginctl" suspend ;;
        esac
    fi
    local busctl="${SDE_BUSCTL_CMD:-busctl}"
    command -v "${busctl%% *}" >/dev/null 2>&1 || return 1
    case "$action" in
        reboot) exec "$busctl" call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager Reboot b false ;;
        shutdown) exec "$busctl" call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager PowerOff b false ;;
        suspend) exec "$busctl" call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager Suspend b false ;;
        logout) exec "$busctl" call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager TerminateSession s "${XDG_SESSION_ID:-}" ;;
    esac
}

do_fallback() {
    local action="$1"
    case "$action" in
        logout)
            # No logind: stopping the supervisor is the log out.
            kill -TERM "$(cat "${SDE_SESSION_PID_FILE}")" 2>/dev/null || return 1
            ;;
        suspend)
            if command -v "${SDE_SUSPEND_CMD:-rtcwake}" >/dev/null 2>&1; then
                exec "${SDE_SUSPEND_CMD:-rtcwake}" -m mem -s 5
            elif command -v pm-suspend >/dev/null 2>&1; then
                exec pm-suspend
            else
                return 1
            fi
            ;;
        *)
            return 1
            ;;
    esac
}

mode="${1:-}"
case "$mode" in
    --list)
        for action in $VALID_ACTIONS; do
            printf 'power.%s=%s\n' "$action" "$(availability "$action" || true)"
        done
        exit 0
        ;;
    --check)
        action="${2:-}"
        is_valid_action "$action" || { echo "sde-power: invalid action '${action}'" >&2; exit 2; }
        availability "$action" || exit 1
        exit 0
        ;;
    --help | "")
        echo "usage: sde-power.sh {logout|reboot|shutdown|suspend} | --check <action> | --list" >&2
        exit 2
        ;;
    *)
        is_valid_action "$mode" || { echo "sde-power: invalid action '${mode}'" >&2; exit 2; }
        action="$mode"
        ;;
esac

if [ -n "${SDE_POWER_CMD:-}" ]; then
    exec "$SDE_POWER_CMD" "$action"
fi
if logind_present; then
    do_logind "$action"
fi
if fallback_present "$action"; then
    do_fallback "$action"
    exit $?
fi
echo "sde-power: '${action}' is unavailable in this environment (no logind and no fallback)" >&2
exit 3
