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
PORTAL="$SDE_ROOT/build/tools/xdg-desktop-portal-serenity"

"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if [ "${1:-}" != "--inner" ]; then
    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-usb.XXXXXX")"
    trap 'rm -rf "$RUNTIME"' EXIT
    export SDE_USB_TEST_RUNTIME="$RUNTIME"
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
if grep -qF "usb-permissions" "$RUNTIME/portal-ask-allow.log" 2>/dev/null || [ -s "$RUNTIME/usb-permissions" ]; then
    echo "  ok: the grant was remembered ($(cat "$RUNTIME/usb-permissions" 2>/dev/null | tr '\n' ' '))"
else
    echo "  MISSING: no usb-permissions record"
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
