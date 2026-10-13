#!/usr/bin/env bash
# Seat-identity helpers (R4.3/R6 multi-seat), sourced by the session scripts.
#
# SerenityOS has no multi-seat model; SDE makes its Wayland session seat-aware so
# it behaves well under a display manager, logind or a second seat. Apps and
# portals read XDG_SEAT/XDG_VTNR, and the runtime namespaces (Wayland socket, the
# supervisor pid file, the /bin shadow dir, the WindowManagerServer socket) are
# per seat so two seats for one user do not collide. seat0 keeps the bare names
# for compatibility.

# Detect and export XDG_SEAT (default seat0) and XDG_VTNR (logind, else the active
# VT). Inherits values already set by the display manager.
sde_detect_seat() {
    if [ -z "${XDG_SEAT:-}" ] && [ -n "${XDG_SESSION_ID:-}" ] && command -v loginctl >/dev/null 2>&1; then
        XDG_SEAT="$(loginctl show-session "$XDG_SESSION_ID" -p Seat --value 2>/dev/null || true)"
    fi
    XDG_SEAT="${XDG_SEAT:-seat0}"
    export XDG_SEAT

    if [ -z "${XDG_VTNR:-}" ] && [ -n "${XDG_SESSION_ID:-}" ] && command -v loginctl >/dev/null 2>&1; then
        XDG_VTNR="$(loginctl show-session "$XDG_SESSION_ID" -p VTNr --value 2>/dev/null || true)"
    fi
    if [ -z "${XDG_VTNR:-}" ]; then
        local active_vt
        active_vt="$(cat /sys/class/tty/tty0/active 2>/dev/null || true)"
        case "$active_vt" in
            tty[0-9]*) XDG_VTNR="${active_vt#tty}" ;;
        esac
    fi
    case "${XDG_VTNR:-}" in
        ''|*[!0-9]*) ;;
        *) export XDG_VTNR ;;
    esac
}

# The runtime-name suffix for the current seat: empty for seat0, "-<seat>"
# otherwise (e.g. "-seat1").
sde_seat_suffix() {
    case "${XDG_SEAT:-seat0}" in
        seat0 | "") printf '' ;;
        *) printf -- '-%s' "$XDG_SEAT" ;;
    esac
}

# The WindowManagerServer socket path for the current seat (the compositor and the
# port's clients both honour SDE_WM_PORTAL_PATH; seat0 keeps the global default).
sde_wm_portal_path() {
    local suffix
    suffix="$(sde_seat_suffix)"
    if [ -n "$suffix" ]; then
        printf '/tmp/portal/wm%s' "$suffix"
    else
        printf '/tmp/portal/wm'
    fi
}
