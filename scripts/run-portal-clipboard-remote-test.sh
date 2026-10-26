#!/usr/bin/env bash
# ci-tier: full
# Portal Clipboard + RemoteDesktop regression test.
#
# Starts a private headless compositor and the SDE portal backend, then drives
# the backend interfaces directly: RemoteDesktop session creation, clipboard
# opt-in, Start's clipboard_enabled result, ConnectToEIS, and Clipboard method
# validation. This is the interface-level companion to the live/client tests.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
PORTAL="$SDE_ROOT/build/tools/xdg-desktop-portal-serenity"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if [[ "${1:-}" != "--inner" ]]; then
    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-clipboard-portal.XXXXXX")"
    mkdir -p "$RUNTIME/xdg-data"
    trap 'rm -rf "$RUNTIME"' EXIT
    export SDE_CLIPBOARD_TEST_RUNTIME="$RUNTIME"
    export XDG_DATA_HOME="$RUNTIME/xdg-data"
    export XDG_DATA_DIRS="$RUNTIME/xdg-data"
    exec dbus-run-session -- "$0" --inner
fi

RUNTIME="$SDE_CLIPBOARD_TEST_RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
SOCK="sde-clipboard-$$"
DEST="org.freedesktop.impl.portal.desktop.serenity"
OBJ="/org/freedesktop/portal/desktop"
REQ="/org/freedesktop/portal/desktop/request/1/clipboard"
SESH="/org/freedesktop/portal/desktop/session/1/clipboard"

cleanup() {
    [[ -n "${PORTAL_PID:-}" ]] && kill "$PORTAL_PID" 2>/dev/null || true
    [[ -n "${COMP_PID:-}" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "${PORTAL_PID:-}" ]] && wait "$PORTAL_PID" 2>/dev/null || true
    [[ -n "${COMP_PID:-}" ]] && wait "$COMP_PID" 2>/dev/null || true
    rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME" SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
if [[ ! -e "$XDG_RUNTIME_DIR/$SOCK" ]]; then
    echo "FAIL: compositor socket was not created"
    tail -20 "$RUNTIME/comp.log"
    exit 1
fi

env WAYLAND_DISPLAY="$SOCK" SDE_STATE_DIR="$RUNTIME" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$PORTAL" > "$RUNTIME/portal.log" 2>&1 &
PORTAL_PID=$!
for _ in $(seq 1 80); do
    gdbus call --session --dest "$DEST" --object-path "$OBJ" \
        --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1 && break
    sleep 0.1
done

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1 (got: $2)"
        fail=1
    fi
}
call() { gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$@" 2>&1; }

check "org.freedesktop.impl.portal.Clipboard" "$(grep -F 'org.freedesktop.impl.portal.Clipboard' "$SDE_ROOT/Base/xdg-desktop-portal/serenity.portal")"
check "org.freedesktop.portal.Clipboard=serenity;gtk" "$(grep -F 'org.freedesktop.portal.Clipboard=serenity;gtk' "$SDE_ROOT/Base/xdg-desktop-portal/portals.conf")"

echo "=== interfaces ==="
intro="$(gdbus introspect --session --dest "$DEST" --object-path "$OBJ" 2>&1 || true)"
check "org.freedesktop.impl.portal.Clipboard" "$intro"
check "org.freedesktop.impl.portal.RemoteDesktop" "$intro"
check "<uint32 3>" "$(call org.freedesktop.DBus.Properties.Get org.freedesktop.impl.portal.RemoteDesktop AvailableDeviceTypes)"

echo "=== RemoteDesktop session + clipboard opt-in ==="
check "uint32 0" "$(call org.freedesktop.impl.portal.RemoteDesktop.CreateSession "$REQ" "$SESH" "org.sde.Test" "{}")"
check "uint32 0" "$(call org.freedesktop.impl.portal.RemoteDesktop.SelectDevices "$REQ" "$SESH" "org.sde.Test" "{'types': <uint32 3>}")"
check "()" "$(call org.freedesktop.impl.portal.Clipboard.RequestClipboard "$SESH" "{}")"
start="$(call org.freedesktop.impl.portal.RemoteDesktop.Start "$REQ" "$SESH" "org.sde.Test" "" "{}")"
check "'clipboard_enabled': <true>" "$start"
check "'devices': <uint32 3>" "$start"

echo "=== Clipboard method validation ==="
check "()" "$(call org.freedesktop.impl.portal.Clipboard.SetSelection "$SESH" "{'mime_types': <['text/plain']>}")"
check "InvalidArgs" "$(call org.freedesktop.impl.portal.Clipboard.SelectionRead "$SESH" "application/x-no-such-format")"
check "InvalidArgs" "$(call org.freedesktop.impl.portal.Clipboard.SelectionWrite "$SESH" "999999")"
check "()" "$(call org.freedesktop.impl.portal.Clipboard.SetSelection "$SESH" "{'mime_types': <@as []>}")"

# Exercise the data path with a real ext-data-control selection owner and a
# D-Bus reader that calls Clipboard.SelectionRead.
if command -v wayland-scanner >/dev/null && pkg-config --exists wayland-client dbus-1; then
    DC_DIR="$RUNTIME/clipboard-client"
    mkdir -p "$DC_DIR"
    DC_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/staging/ext-data-control/ext-data-control-v1.xml"
    wayland-scanner client-header "$DC_XML" "$DC_DIR/ext-data-control-v1-client-protocol.h"
    wayland-scanner private-code  "$DC_XML" "$DC_DIR/ext-data-control-v1-protocol.c"
    cc -O2 -I "$DC_DIR" "$SDE_ROOT/Tools/clipboard-owner-test.c" \
        "$DC_DIR/ext-data-control-v1-protocol.c" \
        $(pkg-config --cflags --libs wayland-client) -o "$DC_DIR/owner" 2> "$RUNTIME/owner-build.log" || true
    cc -O2 "$SDE_ROOT/Tools/clipboard-read-test.c" \
        $(pkg-config --cflags --libs dbus-1) -o "$DC_DIR/reader" 2> "$RUNTIME/reader-build.log" || true
    if [[ -x "$DC_DIR/owner" && -x "$DC_DIR/reader" ]]; then
        env -u DISPLAY WAYLAND_DISPLAY="$SOCK" "$DC_DIR/owner" > "$RUNTIME/owner.log" 2>&1 &
        OWNER_PID=$!
        for _ in $(seq 1 50); do grep -q "clipboard-owner-test: ready" "$RUNTIME/owner.log" 2>/dev/null && break; sleep 0.1; done
        if "$DC_DIR/reader" > "$RUNTIME/reader.log" 2>&1; then
            echo "  ok: Clipboard.SelectionRead returned the owner contents"
            sed 's/^/    /' "$RUNTIME/reader.log"
        else
            echo "  FAIL: Clipboard.SelectionRead data path"
            tail -20 "$RUNTIME/reader.log" | sed 's/^/    /'
            fail=1
        fi
        kill "$OWNER_PID" 2>/dev/null || true
        wait "$OWNER_PID" 2>/dev/null || true
    else
        echo "  SKIP: clipboard owner/reader test clients could not be built"
        tail -10 "$RUNTIME/owner-build.log" "$RUNTIME/reader-build.log" 2>/dev/null | sed 's/^/    /'
    fi
else
    echo "  SKIP: clipboard data-path test (wayland-scanner/dbus-1 unavailable)"
fi

echo "=== RemoteDesktop ConnectToEIS ==="
eis="$(call org.freedesktop.impl.portal.RemoteDesktop.ConnectToEIS "$SESH" "org.sde.Test" "{}")"
echo "  ConnectToEIS -> $eis"
check "handed an EIS fd" "$(cat "$RUNTIME/portal.log")"

if pkg-config --exists dbus-1 libei-1.0; then
    EIS_TEST="$RUNTIME/remote-desktop-eis-test"
    cc -O2 "$SDE_ROOT/Tools/remote-desktop-eis-test.c" \
        $(pkg-config --cflags --libs dbus-1 libei-1.0) -o "$EIS_TEST" 2> "$RUNTIME/eis-build.log" || {
        echo "  note: could not build the libei sender test"
        tail -10 "$RUNTIME/eis-build.log" | sed 's/^/    /'
    }
    if [[ -x "$EIS_TEST" ]]; then
        if "$EIS_TEST" > "$RUNTIME/eis-test.log" 2>&1; then
            echo "  ok: libei sender connected to the EIS endpoint"
            grep -F 'remote-desktop-eis-test:' "$RUNTIME/eis-test.log" | sed 's/^/    /'
        else
            echo "  FAIL: libei sender test failed"
            tail -20 "$RUNTIME/eis-test.log" | sed 's/^/    /'
            fail=1
        fi
    fi
else
    echo "  SKIP: libei sender test (pkg-config dbus-1/libei-1.0 unavailable)"
fi
if grep -qiE 'SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED' "$RUNTIME/portal.log" "$RUNTIME/comp.log"; then
    echo "  FAIL: a process faulted"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- portal log ---"; tail -60 "$RUNTIME/portal.log"
    echo "--- comp log ---"; tail -40 "$RUNTIME/comp.log"
    echo "==> portal clipboard/remote test FAILED"
    exit 1
fi
echo "==> portal clipboard/remote test PASSED"
