#!/usr/bin/env bash
# SDE session supervisor (R4.2, docs/11-r4-implementation-plan.md).
#
# Starts the compositor, launches the shell and the user's XDG Autostart entries,
# and restarts the compositor if it faults (so one crash does not end the
# session). The shell is relaunched by the compositor's own `-s` startup command
# on restart; the compositor reconnects its Wayland clients.
#
# Interoperable by construction (P8/P9): it uses only the XDG Autostart spec and
# the compositor's CLI, and power actions go through logind (`sde-power.sh`).
#
# Environment seams (for embedding and tests):
#   SDE_SESSION_COMPOSITOR   command to run as the compositor (default below)
#   SDE_SESSION_SHELL        shell startup command (only used for the default)
#   SDE_SESSION_APP_BIN_DIR  session app directory, where the apps and the SDE
#                            services are resolved from (default: a per-seat dir
#                            under XDG_RUNTIME_DIR). See the SDE_APP_BIN_DIR block.
#   SDE_SESSION_AUTOSTART_DIR  autostart dir (default $XDG_CONFIG_HOME/autostart)
#   SDE_SESSION_MAX_RESTARTS   restart budget on non-zero exit (default 3)
#   SDE_SESSION_RESTART_DELAY  seconds between restarts (default 1)
#   SDE_SESSION_AUTOSTART     0 to skip autostart (default 1)
#   SDE_SESSION_WATCHDOG_SECONDS  heartbeat staleness before restarting (default 10; 0 disables)
#   SDE_SESSION_HEARTBEAT_FILE    liveness file the compositor writes (default below)
#   SDE_SESSION_COMPOSITOR_SAFE   compositor command for safe mode (minimal session)
#   SDE_SESSION_SAFE_MODE         1 to start in safe mode (default 0)
#   SDE_SESSION_SLEEP_BIN         suspend/resume listener (default scripts/sde-sleep.sh)
#   SDE_SUSPEND_LOCK_ON / SDE_SUSPEND_HOOK / SDE_RESUME_HOOK  suspend/resume hooks
#   SDE_SESSION_CONF             policy file (default $XDG_CONFIG_HOME/sde/session.conf)
#
# The user-facing policy above (idle/lock, restart budget, watchdog, safe mode,
# autostart) can also live in session.conf (KEY=value); the environment overrides
# the file, which overrides the built-in default.
#
# Seat (R4.3/R6 multi-seat): XDG_SEAT/XDG_VTNR come from the display manager,
# logind or the active VT (scripts/sde-seat.sh). The runtime names are namespaced
# per seat -- SDE_SESSION_DISPLAY (Wayland socket) and SDE_WM_PORTAL_PATH (the
# WindowManagerServer socket), plus the pid file and the /bin shadow dir; seat0
# keeps the bare names.
set -u

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

# Session policy (R6): $XDG_CONFIG_HOME/sde/session.conf (KEY=value lines), the
# session sibling of WindowServer.ini (same state directory; see
# docs/14-r4.4-config.md §1). Precedence is environment > file > built-in default,
# so tests and embedding can force a value without touching the file.
session_state_dir() {
    if [ -n "${SDE_STATE_DIR:-}" ]; then printf '%s' "$SDE_STATE_DIR"
    elif [ -n "${XDG_CONFIG_HOME:-}" ]; then printf '%s/sde' "$XDG_CONFIG_HOME"
    elif [ -n "${HOME:-}" ]; then printf '%s/.config/sde' "$HOME"
    else printf '%s' /tmp/sde
    fi
}
# Assign a session variable from the file only when the environment did not set it.
apply_session_setting() {
    local var="$1" value="$2"
    if eval "[ -n \"\${$var+x}\" ]"; then
        return 0
    fi
    printf -v "$var" '%s' "$value"
}
SESSION_CONF="${SDE_SESSION_CONF:-$(session_state_dir)/session.conf}"
if [ -r "$SESSION_CONF" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in ''|'#'*) continue ;; esac
        case "$line" in *=*) ;; *) continue ;; esac
        key="$(printf '%s' "${line%%=*}" | tr -d '[:blank:]')"
        value="$(printf '%s' "${line#*=}" | sed -E 's/^[[:blank:]]+//; s/[[:blank:]]+$//')"
        case "$key" in
            idle_seconds) apply_session_setting SDE_SESSION_IDLE_SECONDS "$value" ;;
            idle_dpms_seconds) apply_session_setting SDE_SESSION_IDLE_DPMS_SECONDS "$value" ;;
            lock_command) apply_session_setting SDE_LOCK_CMD "$value" ;;
            max_restarts) apply_session_setting SDE_SESSION_MAX_RESTARTS "$value" ;;
            restart_delay) apply_session_setting SDE_SESSION_RESTART_DELAY "$value" ;;
            watchdog_seconds) apply_session_setting SDE_SESSION_WATCHDOG_SECONDS "$value" ;;
            safe_mode) apply_session_setting SDE_SESSION_SAFE_MODE "$value" ;;
            autostart) apply_session_setting SDE_SESSION_AUTOSTART "$value" ;;
            autostart_dir) apply_session_setting SDE_SESSION_AUTOSTART_DIR "$value" ;;
            lock_on_suspend) apply_session_setting SDE_SUSPEND_LOCK_ON "$value" ;;
            suspend_hook) apply_session_setting SDE_SUSPEND_HOOK "$value" ;;
            resume_hook) apply_session_setting SDE_RESUME_HOOK "$value" ;;
            *) echo "SDE session: $SESSION_CONF: ignoring unknown key '$key'" >&2 ;;
        esac
    done < "$SESSION_CONF"
    echo "SDE session: loaded session policy from $SESSION_CONF"
fi

# R4.3/R6 multi-seat: seat identity (see scripts/sde-seat.sh). Apps and portals
# use XDG_SEAT/XDG_VTNR; the runtime namespaces below are per seat so two seats
# for one user do not collide.
# shellcheck source=scripts/sde-seat.sh
. "$SDE_ROOT/scripts/sde-seat.sh"
sde_detect_seat
SEAT_SUFFIX="$(sde_seat_suffix)"
echo "SDE session: seat '$XDG_SEAT'${XDG_VTNR:+ vt$XDG_VTNR}"

# The compositor's Wayland socket is per seat (seat0 keeps the bare name).
SESSION_DISPLAY="${SDE_SESSION_DISPLAY:-sde$SEAT_SUFFIX}"
# Export the effective session display (apps/services inherit it; the compositor
# also sets WAYLAND_DISPLAY to it for its children).
export SDE_SESSION_DISPLAY="$SESSION_DISPLAY"
# Multi-seat: the WindowManagerServer socket is global in SerenityOS, so a
# non-default seat gets its own path. Both the compositor and the port's clients
# honour SDE_WM_PORTAL_PATH; seat0 keeps /tmp/portal/wm.
if [ -z "${SDE_WM_PORTAL_PATH:-}" ] && [ -n "$SEAT_SUFFIX" ]; then
    SDE_WM_PORTAL_PATH="$(sde_wm_portal_path)"
fi
if [ -n "${SDE_WM_PORTAL_PATH:-}" ]; then
    export SDE_WM_PORTAL_PATH
fi

SDE_SESSION_SHELL="${SDE_SESSION_SHELL:-$BUILD/bin/Taskbar & $BUILD/bin/FileManager -d}"
COMPOSITOR="${SDE_SESSION_COMPOSITOR:-$SDE_ROOT/build/WindowServer/WindowServer -n $SESSION_DISPLAY -s \"$SDE_SESSION_SHELL\"}"
AUTOSTART_DIR="${SDE_SESSION_AUTOSTART_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/autostart}"
MAX_RESTARTS="${SDE_SESSION_MAX_RESTARTS:-3}"
RESTART_DELAY="${SDE_SESSION_RESTART_DELAY:-1}"
RUN_AUTOSTART="${SDE_SESSION_AUTOSTART:-1}"

# R6/N4 hardening. The watchdog restarts a *wedged* compositor (one whose event
# loop is blocked) by watching a liveness heartbeat file the compositor writes;
# safe mode is a minimal session used when the compositor keeps failing, so a bad
# applet/autostart entry cannot wedge the login loop.
WATCHDOG_SECONDS="${SDE_SESSION_WATCHDOG_SECONDS:-10}"
SAFE_MODE="${SDE_SESSION_SAFE_MODE:-0}"
SAFE_COMPOSITOR="${SDE_SESSION_COMPOSITOR_SAFE:-}"
HEARTBEAT_FILE="${SDE_SESSION_HEARTBEAT_FILE:-${XDG_RUNTIME_DIR:-/tmp}/sde-heartbeat}"
# The compositor (and its clients) inherit this and touch the file each second.
export SDE_HEARTBEAT_FILE="$HEARTBEAT_FILE"

# Idle-to-lock policy (R4.1/R4.2): if SDE_SESSION_IDLE_SECONDS > 0, run the
# portable sde-idle daemon to lock after that many idle seconds. sde-idle is a
# plain ext-idle-notify-v1 client, so this works under any compositor.
IDLE_SECONDS="${SDE_SESSION_IDLE_SECONDS:-0}"
IDLE_BIN="${SDE_SESSION_IDLE_BIN:-$SDE_ROOT/build/tools/sde-idle}"
LOCK_CMD="${SDE_LOCK_CMD:-swaylock}"
DPMS_BIN="${SDE_SESSION_DPMS_BIN:-$SDE_ROOT/build/tools/sde-dpms}"
DPMS_SECONDS="${SDE_SESSION_IDLE_DPMS_SECONDS:-0}"
IDLE_PID=""

# R4.3/R6 suspend/resume: start a portable logind PrepareForSleep listener (locks
# on suspend, re-applies state on resume); a no-op without logind/D-Bus.
SLEEP_BIN="${SDE_SESSION_SLEEP_BIN:-$SDE_ROOT/scripts/sde-sleep.sh}"
SLEEP_PID=""

# Session environment (R4.3, docs/12). Set the standard variables so apps and
# portals can find the session; the supervisor owns ordering (no systemd target).
# (SESSION_DISPLAY is set above, from the seat.)
# Force the session identity: even when nested inside another desktop, the
# session IS SDE (the host's XDG_CURRENT_DESKTOP/DESKTOP_SESSION must not leak).
export XDG_SESSION_TYPE="wayland"
export XDG_SESSION_CLASS="user"
export XDG_CURRENT_DESKTOP="SDE"
export XDG_SESSION_DESKTOP="SDE"
export DESKTOP_SESSION="sde"
# The display the *compositor* connects to: empty means "go native" (DRM+libinput
# via wlr_backend_autocreate); a host socket (e.g. wayland-0) means "nest". Capture
# it BEFORE pointing the rest of the session at our own socket, or the compositor
# inherits WAYLAND_DISPLAY=sde and wrongly selects the nested Wayland backend
# (R4.3 regression: "Could not connect to remote display").
COMPOSITOR_DISPLAY="${WAYLAND_DISPLAY:-}"
# Everything the session starts (apps, services, autostart, idle daemon) talks to
# the compositor's own socket, not the host's.
export WAYLAND_DISPLAY="$SESSION_DISPLAY"

# A pid file so the power helper's portable logout fallback can stop us even
# without logind.
export SDE_SESSION_PID_FILE="${SDE_SESSION_PID_FILE:-${XDG_RUNTIME_DIR:-/tmp}/sde-session$SEAT_SUFFIX.pid}"
echo $$ > "$SDE_SESSION_PID_FILE"
trap 'rm -f "$SDE_SESSION_PID_FILE"' EXIT

# Best-effort: publish the session environment to the D-Bus activation
# environment so D-Bus-activated services (portals) see a Wayland session. This
# needs neither systemd nor the helper to be present; failure is ignored.
push_dbus_activation_env() {
    [ -n "${SDE_DBUS_ENV_CMD:-}" ] || command -v dbus-update-activation-environment >/dev/null 2>&1 || return 0
    local cmd="${SDE_DBUS_ENV_CMD:-dbus-update-activation-environment}"
    # Only pass variables that are actually set (an unset name makes
    # dbus-update-activation-environment fail).
    local vars="XDG_SESSION_TYPE XDG_SESSION_CLASS XDG_CURRENT_DESKTOP XDG_SESSION_DESKTOP DESKTOP_SESSION WAYLAND_DISPLAY XDG_SEAT"
    [ -n "${XDG_VTNR:-}" ] && vars="$vars XDG_VTNR"
    [ -n "${XDG_SESSION_ID:-}" ] && vars="$vars XDG_SESSION_ID"
    # shellcheck disable=SC2086
    WAYLAND_DISPLAY="$SESSION_DISPLAY" "$cmd" $vars >/dev/null 2>&1 || true
}
push_dbus_activation_env

run_desktop_entry() {
    local entry="$1" line="" exec_line="" hidden="" enabled=""
    while IFS= read -r line || [ -n "$line" ]; do
        line="${line%$'\r'}"
        case "$line" in
            Exec=*) exec_line="${line#Exec=}" ;;
            Hidden=*) hidden="${line#Hidden=}" ;;
            X-GNOME-Autostart-enabled=*) enabled="${line#X-GNOME-Autostart-enabled=}" ;;
        esac
    done < "$entry"

    [ "$hidden" = "true" ] && return 0
    [ "$enabled" = "false" ] && return 0
    [ -n "$exec_line" ] || return 0

    # Strip field codes (%u %U %f %F %d %D %n %N %i %c %k %v %m).
    local cmd
    cmd="$(printf '%s' "$exec_line" | sed -E 's/[[:space:]]*%[a-zA-Z]//g')"
    echo "SDE session: autostart $(basename "$entry"): $cmd"
    setsid sh -c "$cmd" >/dev/null 2>&1 &
}

run_autostart() {
    [ "$RUN_AUTOSTART" = "1" ] || return 0
    [ -d "$AUTOSTART_DIR" ] || return 0
    local entry
    for entry in "$AUTOSTART_DIR"/*.desktop; do
        [ -e "$entry" ] || continue
        run_desktop_entry "$entry"
    done
}

# Wait (bounded) for the compositor's Wayland socket before launching clients.
wait_for_socket() {
    local socket="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SESSION_DISPLAY"
    local i
    for i in $(seq 1 50); do
        [ -e "$socket" ] && return 0
        sleep 0.1
    done
    return 1
}

# A liveness watchdog (R6/N4): if the compositor's heartbeat file goes stale its
# event loop is blocked, so terminate it and let the main loop restart it. Runs in
# the background so the foreground `wait` keeps the fast SIGTERM path.
watch_compositor_hang() {
    local pid="$1" beat now
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        [ -e "$HEARTBEAT_FILE" ] || continue
        beat="$(cat "$HEARTBEAT_FILE" 2>/dev/null || echo 0)"
        now="$(date +%s)"
        case "$beat" in
            ''|*[!0-9]*) continue ;;
        esac
        if [ "$((now - beat))" -gt "$WATCHDOG_SECONDS" ]; then
            echo "SDE session: compositor heartbeat stale (${beat}s, now ${now}s > ${WATCHDOG_SECONDS}s); restarting" >&2
            kill -TERM "$pid" 2>/dev/null || true
            sleep 2
            kill -KILL "$pid" 2>/dev/null || true
            return
        fi
    done
}

# Power helpers: shadow /bin/{shutdown,reboot,logout,suspend} with sde-power, so
# the unmodified Taskbar "Exit..." menu (whose ShutdownDialog spawns
# /bin/shutdown, /bin/reboot, /bin/logout) routes to logind instead of failing on
# host binaries. The port's Process::resolve_executable_path() consults
# SDE_SESSION_BIN_DIR first. Serenity-stdlib convention, standard backend.
SESSION_BIN="${SDE_SESSION_BIN_DIR:-${XDG_RUNTIME_DIR:-/tmp}/sde-session-bin$SEAT_SUFFIX}"
mkdir -p "$SESSION_BIN"
for action in shutdown reboot logout suspend; do
    cat > "$SESSION_BIN/$action" <<EOF
#!/bin/sh
exec "$SDE_ROOT/scripts/sde-power.sh" $action "\$@"
EOF
    chmod +x "$SESSION_BIN/$action"
done
export SDE_SESSION_BIN_DIR="$SESSION_BIN"
echo "SDE session: power helpers in $SESSION_BIN (/bin shadow)"

# Session app directory (SDE-owned). libgui-wayland is a compositor-agnostic
# client stack, so it does not build the session's out-of-process services: SDE
# builds them (WebContent, RequestServer, SQLServer, NotificationServer) and owns
# the directory everything resolves "/bin/X" through. LibWM and the LaunchServer
# resolve service and application binaries from SDE_APP_BIN_DIR, so it must hold
# both: link the port's applications here and overlay the SDE services. See
# docs/06-service-layering.md and docs/07-systemserver-roles.md (section 2.3).
SESSION_SERVICES=(WebContent RequestServer SQLServer NotificationServer AudioServer)
missing_services=0
for service in "${SESSION_SERVICES[@]}"; do
    [ -x "$SDE_ROOT/build/$service/$service" ] || missing_services=1
done
if [ "$missing_services" = "1" ]; then
    echo "SDE session: building the session services (${SESSION_SERVICES[*]})"
    "$SDE_ROOT/scripts/build-services.sh" >/dev/null 2>&1 \
        || echo "note: session services not built; WebView apps and notifications may fail" >&2
fi

APP_BIN="${SDE_SESSION_APP_BIN_DIR:-${XDG_RUNTIME_DIR:-/tmp}/sde-app-bin$SEAT_SUFFIX}"
mkdir -p "$APP_BIN"
for app in "$BUILD/bin"/*; do
    [ -e "$app" ] || continue
    name="$(basename "$app")"
    [ -e "$APP_BIN/$name" ] || ln -s "$app" "$APP_BIN/$name"
done
# SDE's own services take precedence over any same-named port binary.
for service in "${SESSION_SERVICES[@]}"; do
    if [ -x "$SDE_ROOT/build/$service/$service" ]; then
        ln -sf "$SDE_ROOT/build/$service/$service" "$APP_BIN/$service"
    fi
done
export SDE_APP_BIN_DIR="$APP_BIN"
echo "SDE session: session app directory $APP_BIN"

restarts=0
SAFE=0
active_compositor="$COMPOSITOR"
enter_safe_mode() {
    SAFE=1
    echo "SDE session: SAFE MODE (minimal session: no autostart, no applets)" >&2
    RUN_AUTOSTART=0
    if [ -n "$SAFE_COMPOSITOR" ]; then
        active_compositor="$SAFE_COMPOSITOR"
    fi
}
if [ "$SAFE_MODE" = "1" ]; then
    enter_safe_mode
fi

while :; do
    # Start from a clean heartbeat: while the compositor is starting up it has not
    # written one yet, and a stale file from the previous run must not look hung.
    rm -f "$HEARTBEAT_FILE" 2>/dev/null || true
    echo "SDE session: starting compositor: $active_compositor"
    # Native (no captured host display): leave WAYLAND_DISPLAY unset so
    # wlr_backend_autocreate selects DRM+libinput. Nested: connect to the host.
    # setsid puts the compositor (and the -s shell and apps it spawns) in their own
    # process group, so a restart can sweep them (below) instead of orphaning them.
    if [ -n "$COMPOSITOR_DISPLAY" ]; then
        setsid env WAYLAND_DISPLAY="$COMPOSITOR_DISPLAY" sh -c "$active_compositor" &
    else
        setsid env -u WAYLAND_DISPLAY -u DISPLAY sh -c "$active_compositor" &
    fi
    comp_pid=$!

    # Idle-to-lock (optional): wait briefly for the compositor socket, then arm
    # the portable idle daemon against it.
    if [ "$IDLE_SECONDS" -gt 0 ] && [ -x "$IDLE_BIN" ]; then
        sleep 0.3
        if [ "$DPMS_SECONDS" -gt 0 ] && [ -x "$DPMS_BIN" ]; then
            # Blank (DPMS off) before locking; restore on activity.
            "$IDLE_BIN" timeout "$DPMS_SECONDS" "$DPMS_BIN off" timeout "$IDLE_SECONDS" "$LOCK_CMD" resume "$DPMS_BIN on" >/dev/null 2>&1 &
            echo "SDE session: blank at ${DPMS_SECONDS}s, lock at ${IDLE_SECONDS}s via '$LOCK_CMD'"
        else
            "$IDLE_BIN" timeout "$IDLE_SECONDS" "$LOCK_CMD" >/dev/null 2>&1 &
            echo "SDE session: idle-to-lock after ${IDLE_SECONDS}s via '$LOCK_CMD'"
        fi
        IDLE_PID=$!
    fi

    # R4.3/R6: watch logind's PrepareForSleep (lock on suspend; re-apply state on
    # resume). Started here, killed below, so it lives with the compositor.
    if [ -x "$SLEEP_BIN" ]; then
        "$SLEEP_BIN" >/dev/null 2>&1 &
        SLEEP_PID=$!
    fi

    # Launch the XDG Autostart entries once the compositor's socket is up. Inside
    # the loop so they are relaunched after a compositor restart (they were
    # disconnected when the old compositor exited), and never before the socket
    # exists. The shell is (re)launched by the compositor's own -s command.
    if [ "$RUN_AUTOSTART" = "1" ]; then
        wait_for_socket || true
        run_autostart
    fi

    # A SIGTERM/SIGINT means "stop the session", not "restart the compositor":
    # kill it and break out of the loop (this is how logout shuts the session
    # down). Single quotes so comp_pid/IDLE_PID resolve when the signal arrives.
    stopping=0
    # shellcheck disable=SC2064
    trap 'stopping=1; kill -TERM $comp_pid $IDLE_PID $SLEEP_PID 2>/dev/null || true' TERM INT

    # Watchdog: restart a wedged compositor (event loop blocked). Backgrounded so
    # the blocking `wait` below stays responsive to SIGTERM.
    watchdog_pid=""
    if [ "$WATCHDOG_SECONDS" -gt 0 ]; then
        watch_compositor_hang "$comp_pid" &
        watchdog_pid=$!
    fi

    wait "$comp_pid"
    status=$?
    trap - TERM INT
    # Sweep the compositor's process group: the -s shell and the apps it spawned are
    # in it, and when the compositor exits (crash or SIGTERM) they would otherwise
    # be orphaned, so a restart left a duplicate Taskbar/Desktop/applets running.
    kill -TERM -- "-$comp_pid" 2>/dev/null || true
    sleep 0.2
    kill -KILL -- "-$comp_pid" 2>/dev/null || true
    if [ -n "$watchdog_pid" ]; then
        kill "$watchdog_pid" 2>/dev/null || true
    fi
    if [ -n "$IDLE_PID" ]; then
        kill "$IDLE_PID" 2>/dev/null || true
        IDLE_PID=""
    fi
    if [ -n "$SLEEP_PID" ]; then
        kill "$SLEEP_PID" 2>/dev/null || true
        SLEEP_PID=""
    fi

    if [ "$stopping" -eq 1 ]; then
        echo "SDE session: stopping"
        break
    fi
    if [ "$status" -eq 0 ]; then
        echo "SDE session: compositor exited; stopping"
        break
    fi

    restarts=$((restarts + 1))
    if [ "$restarts" -gt "$MAX_RESTARTS" ]; then
        if [ "$SAFE" -eq 0 ]; then
            # Rather than ending the session, drop to a minimal one.
            echo "SDE session: compositor failed $restarts time(s); entering safe mode" >&2
            enter_safe_mode
            restarts=0
            sleep "$RESTART_DELAY"
            continue
        fi
        echo "SDE session: compositor failed $restarts time(s) in safe mode; giving up" >&2
        exit 1
    fi
    echo "SDE session: compositor exited ($status); restart $restarts/$MAX_RESTARTS"
    sleep "$RESTART_DELAY"
done
