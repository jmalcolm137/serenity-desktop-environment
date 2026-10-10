#!/usr/bin/env bash
# SDE session power helper (R4.2, docs/11-r4-implementation-plan.md): route the
# session power actions to the portable mechanism — logind / systemd-logind on
# D-Bus (org.freedesktop.login1) — instead of SerenityOS's /bin/shutdown helpers.
#
# Usage: sde-power {logout|reboot|shutdown|suspend}
#
# For tests and wrappers, SDE_POWER_CMD replaces the real action with a command
# that receives the action name.
set -eu

action="${1:-}"
case "$action" in
    logout | reboot | shutdown | suspend) ;;
    *)
        echo "usage: sde-power {logout|reboot|shutdown|suspend}" >&2
        exit 2
        ;;
esac

if [ -n "${SDE_POWER_CMD:-}" ]; then
    exec "$SDE_POWER_CMD" "$action"
fi

loginctl_action() {
    case "$1" in
        logout) echo terminate-session ;;
        reboot) echo reboot ;;
        shutdown) echo poweroff ;;
        suspend) echo suspend ;;
    esac
}

if command -v loginctl >/dev/null 2>&1; then
    if [ "$action" = "logout" ]; then
        session="${XDG_SESSION_ID:-$(loginctl --no-legend 2>/dev/null | awk -v u="$USER" '$3==u {print $1; exit}')}"
        if [ -n "$session" ]; then
            exec loginctl terminate-session "$session"
        fi
        echo "sde-power: no session to log out" >&2
        exit 1
    fi
    exec loginctl "$(loginctl_action "$action")"
fi

# Fallback: talk to logind directly.
if command -v busctl >/dev/null 2>&1; then
    case "$action" in
        reboot) exec busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager Reboot b false ;;
        shutdown) exec busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager PowerOff b false ;;
        suspend) exec busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager Suspend b false ;;
        logout) exec busctl call org.freedesktop.login1 /org/freedesktop/login1 org.freedesktop.login1.Manager TerminateSession s "${XDG_SESSION_ID:-}" ;;
    esac
fi

echo "sde-power: no logind (loginctl/busctl) available" >&2
exit 1
