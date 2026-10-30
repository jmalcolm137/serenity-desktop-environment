#!/usr/bin/env bash
# Headless test for the USB portal backend (R5, docs/15).
#
# The freedesktop USB portal's frontend (xdg-desktop-portal) enumerates the
# connected devices with udev and opens the device node for the app; this backend
# is the *broker* that decides whether the app may have it
# (org.freedesktop.impl.portal.Usb.AcquireDevices).
#
# This runs the backend on a private session bus and drives it directly, checking
# each policy: allow (grant without asking), deny (refuse), and ask (prompt
# through the LibGUI AccessDialog, then remember the grant so a device is not
# asked about twice).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
PORTAL="$SDE_ROOT/build/tools/xdg-desktop-portal-serenity"

"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if [ "${1:-}" != "--inner" ]; then
    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-usb.XXXXXX")"
    trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"' EXIT
    export SDE_USB_TEST_RUNTIME="$RUNTIME"
    # Isolate the freedesktop permission store's database (it lands in
    # XDG_DATA_HOME) inside the test runtime, so the store the backend writes to is
    # empty each run and never touches the developer's real grants. Set before the
    # exec so the bus-activated store daemon inherits it.
    export XDG_DATA_HOME="$RUNTIME/data"
    mkdir -p "$XDG_DATA_HOME"
    exec dbus-run-session -- "$0" --inner
fi

RUNTIME="$SDE_USB_TEST_RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
# Keep the portal's state (usb-permissions) inside the test runtime dir.
export SDE_STATE_DIR="$RUNTIME"
DEST="org.freedesktop.impl.portal.desktop.serenity"
OBJ="/org/freedesktop/portal/desktop"
IFACE="org.freedesktop.impl.portal.Usb"
DEVICES="[('usb:1-1', {'device-file': <'/dev/bus/usb/001/002'>}, {'writable': <true>})]"

# A stub AccessDialog: it records each invocation and answers with $ANSWER.
ANSWER_FILE="$RUNTIME/answer"
printf '0' > "$ANSWER_FILE"
DIALOG_LOG="$RUNTIME/dialog.log"
: > "$DIALOG_LOG"
ACCESS_DIALOG="$RUNTIME/accessdialog.sh"
cat > "$ACCESS_DIALOG" <<EOF
#!/usr/bin/env bash
echo "\$@" >> "$DIALOG_LOG"
cat "$ANSWER_FILE"
EOF
chmod +x "$ACCESS_DIALOG"

PORTAL_PID=""
start_portal() {
    env "$@" "$PORTAL" > "$RUNTIME/portal-$CASE.log" 2>&1 &
    PORTAL_PID=$!
    for _ in $(seq 1 50); do
        gdbus call --session --dest "$DEST" --object-path "$OBJ" --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1 && break
        sleep 0.1
    done
}
stop_portal() {
    [[ -n "$PORTAL_PID" ]] && kill "$PORTAL_PID" 2>/dev/null || true
    wait "$PORTAL_PID" 2>/dev/null || true
    PORTAL_PID=""
    sleep 0.2
}
cleanup() { stop_portal; }
trap cleanup EXIT

call() { gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$@" 2>&1; }

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then echo "  ok: $1"; else echo "  MISSING ($2): $1"; fail=1; fi
}

# Query the freedesktop permission store directly (to confirm the backend wrote a
# grant there). Lookup raises an error for an unknown id, so a reachable store and
# a missing one are told apart by the error name, not the exit code.
store_lookup() {
    gdbus call --session --dest org.freedesktop.impl.portal.PermissionStore \
        --object-path /org/freedesktop/impl/portal/PermissionStore \
        --method org.freedesktop.impl.portal.PermissionStore.Lookup "$1" "$2" 2>&1
}
STORE_OK=0
if ! grep -qE "ServiceUnknown|not provided by any" <<<"$(store_lookup usb probe)"; then
    STORE_OK=1
    echo "note: a permission store is on the bus; grants go there (flat file is the fallback)"
else
    echo "note: no permission store on the bus; grants use the flat-file fallback"
fi

echo "=== interface ==="
CASE=base
start_portal SDE_PORTAL_USB=allow
check "uint32 1" "$(call org.freedesktop.DBus.Properties.Get $IFACE version)"
stop_portal

echo "=== SDE_PORTAL_USB=allow ==="
CASE=allow
start_portal SDE_PORTAL_USB=allow
allowed="$(call $IFACE.AcquireDevices /h "" "org.sde.Test" "$DEVICES" "{}")"
stop_portal
check "uint32 0" "$allowed"
check "usb:1-1" "$allowed"
check "writable': <true>" "$allowed"

echo "=== SDE_PORTAL_USB=deny ==="
CASE=deny
start_portal SDE_PORTAL_USB=deny
denied="$(call $IFACE.AcquireDevices /h "" "org.sde.Test" "$DEVICES" "{}")"
stop_portal
check "uint32 1" "$denied"

echo "=== ask: the user allows (and the grant is remembered) ==="
CASE=ask-allow
printf '0' > "$ANSWER_FILE"
start_portal SDE_PORTAL_ACCESS_DIALOG="$ACCESS_DIALOG" SDE_PORTAL_USB=ask
asked="$(call $IFACE.AcquireDevices /h "" "org.sde.Test" "$DEVICES" "{}")"
stop_portal
check "uint32 0" "$asked"
check "usb:1-1" "$asked"
if [ "$STORE_OK" -eq 1 ] && grep -qF "org.sde.Test" <<<"$(store_lookup usb usb:1-1)"; then
    echo "  ok: the grant was remembered in the permission store"
elif [ -s "$RUNTIME/usb-permissions" ]; then
    echo "  ok: the grant was remembered in the flat-file fallback ($(tr '\n' ' ' < "$RUNTIME/usb-permissions"))"
else
    echo "  MISSING: the grant was not remembered"
    fail=1
fi
DIALOG_CALLS="$(wc -l < "$DIALOG_LOG")"
check "shown once" "$([[ "$DIALOG_CALLS" -eq 1 ]] && echo 'shown once' || echo "shown $DIALOG_CALLS times")"

echo "=== ask: a remembered grant is not asked about again ==="
CASE=ask-remembered
printf '1' > "$ANSWER_FILE"
start_portal SDE_PORTAL_ACCESS_DIALOG="$ACCESS_DIALOG" SDE_PORTAL_USB=ask
remembered="$(call $IFACE.AcquireDevices /h "" "org.sde.Test" "$DEVICES" "{}")"
stop_portal
check "uint32 0" "$remembered"
check "usb:1-1" "$remembered"
DIALOG_CALLS_2="$(wc -l < "$DIALOG_LOG")"
check "no new prompt" "$([[ "$DIALOG_CALLS_2" -eq "$DIALOG_CALLS" ]] && echo 'no new prompt' || echo "shown again")"

echo "=== ask: the user denies a new device ==="
CASE=ask-deny
printf '1' > "$ANSWER_FILE"
start_portal SDE_PORTAL_ACCESS_DIALOG="$ACCESS_DIALOG" SDE_PORTAL_USB=ask
refused="$(call $IFACE.AcquireDevices /h "" "org.sde.Other" "[('usb:2-1', {}, {'writable': <false>})]" "{}")"
stop_portal
check "uint32 1" "$refused"

echo "=== migration: a legacy flat-file grant is honoured (and migrated) ==="
CASE=migrate
printf 'org.sde.Legacy\tusb:9-9\t1\n' > "$RUNTIME/usb-permissions"
BEFORE_MIGRATE="$(wc -l < "$DIALOG_LOG")"
start_portal SDE_PORTAL_ACCESS_DIALOG="$ACCESS_DIALOG" SDE_PORTAL_USB=ask
migrated="$(call $IFACE.AcquireDevices /h "" "org.sde.Legacy" "[('usb:9-9', {}, {'writable': <true>})]" "{}")"
stop_portal
check "uint32 0" "$migrated"
AFTER_MIGRATE="$(wc -l < "$DIALOG_LOG")"
check "no prompt for a legacy grant" "$([[ "$AFTER_MIGRATE" -eq "$BEFORE_MIGRATE" ]] && echo 'no prompt for a legacy grant' || echo 'prompted')"
if [ "$STORE_OK" -eq 1 ]; then
    check "org.sde.Legacy" "$(store_lookup usb usb:9-9)"
fi

echo "=== ask without a dialog refuses (no way to prompt) ==="
CASE=ask-nodialog
start_portal SDE_PORTAL_USB=ask
no_dialog="$(call $IFACE.AcquireDevices /h "" "org.sde.Test" "[('usb:3-1', {}, {})]" "{}")"
stop_portal
check "uint32 1" "$no_dialog"

echo "=== an empty request is an error, not a denial ==="
CASE=empty
start_portal SDE_PORTAL_USB=allow
empty="$(call $IFACE.AcquireDevices /h "" "org.sde.Test" "[]" "{}")"
stop_portal
check "uint32 2" "$empty"

if [[ "$fail" -ne 0 ]]; then
    echo "==> usb portal test FAILED"
    exit 1
fi
echo "==> usb portal test PASSED"
