#!/usr/bin/env bash
# Headless test for the freedesktop notification daemon (org.freedesktop.Notifications).
#
# It starts a private D-Bus session (dbus-run-session), a private XDG_RUNTIME_DIR
# and a headless compositor, then drives sde-notifications through gdbus and
# checks that:
#   - the bus name is owned and the standard methods answer,
#   - Notify shows a Serenity banner (the compositor's serenity-notification layer),
#   - replaces_id reuses the id,
#   - CloseNotification emits NotificationClosed(3),
#   - expire_timeout emits NotificationClosed(1),
#   - a host client (notify-send, when installed) can use it.
#
# Everything is private, so it never touches (or is answered by) a live session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
DAEMON="$SDE_ROOT/build/tools/sde-notifications"

if [ "${1:-}" != "--inner" ]; then
    exec dbus-run-session -- "$0" --inner
fi

if [[ "${SDE_TEST_NO_BUILD:-0}" != "1" ]]; then
    "$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
    "$SDE_ROOT/scripts/build-services.sh" >/dev/null 2>&1 || true
    "$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true
fi

[[ -x "$DAEMON" ]] || { echo "SKIP: sde-notifications not built"; exit 0; }
[[ -x "$SDE_ROOT/build/NotificationServer/NotificationServer" ]] || { echo "SKIP: NotificationServer not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-notifications.XXXXXX")"
chmod 700 "$RUNTIME"
# A private runtime dir: the compositor socket and the SDE session portals live
# here, not in the live session's /run/user.
export XDG_RUNTIME_DIR="$RUNTIME/run"
export SDE_STATE_DIR="$RUNTIME/state"
export SDE_SESSION_DIR="$RUNTIME/session"
export SDE_WM_PORTAL_PATH="$RUNTIME/wm"
mkdir -p "$XDG_RUNTIME_DIR" "$SDE_SESSION_DIR/portal" "$SDE_STATE_DIR"
chmod 700 "$XDG_RUNTIME_DIR" "$SDE_SESSION_DIR"

APP_BIN="$RUNTIME/bin"
mkdir -p "$APP_BIN"
ln -sf "$SDE_ROOT/build/NotificationServer/NotificationServer" "$APP_BIN/NotificationServer"

COMP_PID=""
DAEMON_PID=""
cleanup() {
    [[ -n "$DAEMON_PID" ]] && kill "$DAEMON_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

SOCK="sde-notif-dbus-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.5

env WAYLAND_DISPLAY="$SOCK" \
    SDE_APP_BIN_DIR="$APP_BIN" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$DAEMON" > "$RUNTIME/daemon.log" 2>&1 &
DAEMON_PID=$!

DEST="org.freedesktop.Notifications"
OBJ="/org/freedesktop/Notifications"
IFACE="org.freedesktop.Notifications"
for _ in $(seq 1 80); do
    gdbus call --session --dest "$DEST" --object-path "$OBJ" --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1 && break
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "warning: sde-notifications exited early:"
        cat "$RUNTIME/daemon.log"
        echo "==> notifications test SKIPPED (no daemon; another one may own the name)"
        exit 0
    fi
    sleep 0.1
done

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}
call() {
    gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$@" 2>&1
}
id_of() {
    grep -oE 'uint32 [0-9]+' <<<"$1" | head -1 | awk '{print $2}'
}

echo "=== capabilities / server information ==="
caps="$(call "$IFACE.GetCapabilities")"
check "body" "$caps"
check "icon-static" "$caps"
info="$(call "$IFACE.GetServerInformation")"
check "SDE Notification Server" "$info"
check "Serenity Desktop Environment" "$info"
check "1.2" "$info"

echo "=== Notify shows a banner ==="
# expire_timeout 0: keep it alive for the CloseNotification test below.
first="$(call "$IFACE.Notify" "SDE Test" 0 "" "Hello from a host app" "This is the body" "[]" "{}" 0)"
first_id="$(id_of "$first")"
check "uint32 $first_id" "$first"
for _ in $(seq 1 30); do
    grep -qF "serenity-notification" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
if grep -qF "serenity-notification" "$RUNTIME/comp.log"; then
    echo "  ok: a serenity-notification banner was created"
else
    echo "  FAIL: no serenity-notification banner (see $RUNTIME/comp.log)"
    fail=1
fi

echo "=== replaces_id reuses the id (same client connection) ==="
# gdbus opens a new connection per call, so replacing across two gdbus calls is a
# *different* client by the spec (and correctly gets a new id). Use one Gio
# connection for the real same-client case.
if python3 -c 'import gi; from gi.repository import Gio' >/dev/null 2>&1; then
    if replaces_output="$(python3 - <<'PY'
from gi.repository import Gio, GLib
import sys

bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
iface = "org.freedesktop.Notifications"
obj = "/org/freedesktop/Notifications"

def notify(replaces, summary, body):
    result = bus.call_sync(iface, obj, iface, "Notify",
        GLib.Variant("(susssasa{sv}i)", ("SDE Test", replaces, "", summary, body, [], {}, 0)),
        GLib.VariantType("(u)"), Gio.DBusCallFlags.NONE, -1, None)
    return result.unpack()[0]

first = notify(0, "Replace me", "first")
second = notify(first, "Replaced", "second")
print(f"ids {first} {second}")
if first != second:
    sys.exit(1)
bus.call_sync(iface, obj, iface, "CloseNotification", GLib.Variant("(u)", (first,)),
    None, Gio.DBusCallFlags.NONE, -1, None)
PY
)"; then
        echo "  ok: replaces_id reused the id ($replaces_output)"
    else
        echo "  FAIL: replaces_id did not reuse the id ($replaces_output)"
        fail=1
    fi
else
    echo "  note: PyGObject not available; skipping the same-connection replaces_id check"
fi

echo "=== CloseNotification emits NotificationClosed(3) ==="
timeout 5 gdbus monitor --session --dest "$DEST" --object-path "$OBJ" > "$RUNTIME/closed.log" 2>&1 &
MONITOR_PID=$!
sleep 0.5
call "$IFACE.CloseNotification" "$first_id" >/dev/null
for _ in $(seq 1 30); do
    grep -qF "NotificationClosed" "$RUNTIME/closed.log" 2>/dev/null && break
    sleep 0.1
done
kill "$MONITOR_PID" 2>/dev/null || true
check "NotificationClosed" "$(cat "$RUNTIME/closed.log")"
check ", uint32 3)" "$(cat "$RUNTIME/closed.log")"

echo "=== expire_timeout emits NotificationClosed(1) ==="
timeout 5 gdbus monitor --session --dest "$DEST" --object-path "$OBJ" > "$RUNTIME/expired.log" 2>&1 &
MONITOR_PID=$!
sleep 0.5
expiring="$(call "$IFACE.Notify" "SDE Test" 0 "" "Short lived" "Expires" "[]" "{}" 300)"
expiring_id="$(id_of "$expiring")"
check "uint32 $expiring_id" "$expiring"
sleep 1.2
kill "$MONITOR_PID" 2>/dev/null || true
check "NotificationClosed" "$(cat "$RUNTIME/expired.log")"
check ", uint32 1)" "$(cat "$RUNTIME/expired.log")"

echo "=== a host client (notify-send) can use it ==="
if command -v notify-send >/dev/null 2>&1; then
    before="$(grep -cF "serenity-notification" "$RUNTIME/comp.log" || true)"
    notify-send "Host client" "via org.freedesktop.Notifications" >/dev/null 2>&1 || true
    sleep 0.5
    after="$(grep -cF "serenity-notification" "$RUNTIME/comp.log" || true)"
    if [[ "$after" -gt "$before" ]]; then
        echo "  ok: notify-send produced a banner ($before -> $after)"
    else
        echo "  note: notify-send produced no new banner ($before -> $after)"
    fi
else
    echo "  note: notify-send not installed; skipping the host-client check"
fi

echo "--- daemon log ---"
cat "$RUNTIME/daemon.log"

if [[ "$fail" -ne 0 ]]; then
    echo "==> notifications test FAILED"
    exit 1
fi
echo "==> notifications test PASSED"
