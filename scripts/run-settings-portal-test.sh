#!/usr/bin/env bash
# Headless test for the R4.4 Settings portal backend (docs/14-r4.4-config.md §6):
# sde-settings-portal implements org.freedesktop.impl.portal.Settings and reports
# color-scheme / accent-color / contrast from the active SDE theme.
#
# Runs on a private session bus (dbus-run-session), so it does not disturb a live
# session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
PORTAL="$SDE_ROOT/build/tools/sde-settings-portal"

"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if [ "${1:-}" != "--inner" ]; then
    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-portal.XXXXXX")"
    trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT
    export XDG_RUNTIME_DIR="$RUNTIME"
    export SDE_PORTAL_TEST_RUNTIME="$RUNTIME"
    exec dbus-run-session -- "$0" --inner
fi

RUNTIME="$SDE_PORTAL_TEST_RUNTIME"
DEST="org.freedesktop.impl.portal.desktop.sde"
OBJ="/org/freedesktop/portal/desktop"
IFACE="org.freedesktop.impl.portal.Settings"
LIGHT="$SERENITY/Base/res/themes/Redmond 2000.ini"
DARK="$SERENITY/Base/res/themes/Dark.ini"

echo "$LIGHT" > "$RUNTIME/sde-theme"
"$PORTAL" > "$RUNTIME/portal.log" 2>&1 &
PORTAL_PID=$!
trap 'kill "$PORTAL_PID" 2>/dev/null || true' EXIT

for _ in $(seq 1 50); do
    gdbus call --session --dest "$DEST" --object-path "$OBJ" --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1 && break
    sleep 0.1
done

read_setting() {
    gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$IFACE.Read" org.freedesktop.appearance "$1" 2>&1
}

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

echo "=== (A) light theme (Redmond 2000: light, accent #4a6eab) ==="
check "uint32 2" "$(read_setting color-scheme)"
check "0.29" "$(read_setting accent-color)"
check "uint32 0" "$(read_setting contrast)"

echo "=== (B) dark theme ==="
echo "$DARK" > "$RUNTIME/sde-theme"
sleep 0.3
check "uint32 1" "$(read_setting color-scheme)"

echo "=== (C) ReadAll ==="
readall="$(gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$IFACE.ReadAll" org.freedesktop.appearance 2>&1)"
check "color-scheme" "$readall"
check "accent-color" "$readall"
check "contrast" "$readall"

echo "=== (D) Properties.GetAll ==="
allprops="$(gdbus call --session --dest "$DEST" --object-path "$OBJ" --method org.freedesktop.DBus.Properties.GetAll "$IFACE" 2>&1)"
check "'version'" "$allprops"
check "uint32 2" "$allprops"

echo "--- portal log ---"
tail -3 "$RUNTIME/portal.log" || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> settings portal test FAILED"
    exit 1
fi
echo "==> settings portal test PASSED"
