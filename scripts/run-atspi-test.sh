#!/usr/bin/env bash
# Headless AT-SPI test (R5 accessibility): launch a LibGUI app on a private
# D-Bus and walk the accessibility tree the compositor session exposes.
#
# The tree is published by LibGUI through LibAccessibility's Linux backend; no
# screen reader or at-spi registry is required for the read-only step. The app
# is a ComboBox test window: its frame must be a child of the application and
# its name must survive the round trip.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
APP="$BUILD/bin/Magnifier"

if ! command -v gdbus >/dev/null || ! command -v dbus-daemon >/dev/null; then
    echo "SKIP: gdbus/dbus-daemon are not installed"
    exit 0
fi
[[ -x "$APP" ]] || { echo "SKIP: Magnifier not built"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-atspi.XXXXXX")"
chmod 700 "$RUNTIME"
mkdir -p "$RUNTIME/run" "$RUNTIME/session/portal" "$RUNTIME/state"
SOCK="sde-atspi-$$"
COMP_PID=""
APP_PID=""
DBUS_PID=""
DBUS_ADDRESS=""
cleanup() {
    [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$DBUS_PID" ]] && kill "$DBUS_PID" 2>/dev/null || true
    sleep 0.3
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

DBUS_INFO="$(dbus-daemon --session --fork --print-address=1 --print-pid=1)"
DBUS_ADDRESS="$(sed -n '1p' <<<"$DBUS_INFO")"
DBUS_PID="$(sed -n '2p' <<<"$DBUS_INFO")"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 SDE_XWAYLAND=0 \
    XDG_RUNTIME_DIR="$RUNTIME/run" SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do [[ -e "$RUNTIME/run/$SOCK" ]] && break; sleep 0.1; done
sleep 1

env WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME/run" SDE_STATE_DIR="$RUNTIME/state" \
    SDE_SESSION_DIR="$RUNTIME/session" AT_SPI_BUS_ADDRESS="$DBUS_ADDRESS" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 20 "$APP" > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!
sleep 1

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }
call() { gdbus call --address "$DBUS_ADDRESS" --dest "$NAME" --object-path "$1" --method "org.a11y.atspi.Accessible.$2" 2>&1; }

NAME=""
for _ in $(seq 1 40); do
    NAMES="$(gdbus call --address "$DBUS_ADDRESS" --dest org.freedesktop.DBus \
        --object-path /org/freedesktop/DBus --method org.freedesktop.DBus.ListNames 2>/dev/null || true)"
    NAME="$(sed -n 's/.*\(org\.a11y\.atspi\.App_[0-9]*\).*/\1/p' <<<"$NAMES" | head -1)"
    [[ -n "$NAME" ]] && break
    sleep 0.1
done

echo "=== AT-SPI read-only tree ==="
check "an application registered on the accessibility bus" "[[ -n '$NAME' ]]"
if [[ -n "$NAME" ]]; then
    check "the application node has the application role" "grep -qF '(75,)' <<<\"\$(call /org/a11y/atspi/accessible/root GetRole)\""
    CHILDREN="$(call /org/a11y/atspi/accessible/root GetChildren)"
    check "the application has a frame child" "grep -q 'org/a11y/atspi/accessible/' <<<\"\$CHILDREN\""
    FRAME_PATH="$(sed -n 's/.*objectpath '\''\([^'\'']*\)'\''.*/\1/p' <<<\"$CHILDREN\" | head -1)"
    check "a frame child was returned" "[[ -n '$FRAME_PATH' ]]"
    if [[ -n "$FRAME_PATH" ]]; then
        check "the frame has the frame role" "grep -qF '(23,)' <<<\"\$(call '$FRAME_PATH' GetRole)\""
        check "the frame keeps the window title" "grep -qF \"'Magnifier'\" <<<\"\$(call '$FRAME_PATH' GetName)\""
        check "the frame exposes the Component interface" "grep -qF \"'org.a11y.atspi.Component'\" <<<\"\$(call '$FRAME_PATH' GetInterfaces)\""
        check "the frame reports non-empty extents" "! grep -qF '(0, 0, 0, 0)' <<<\"\$(call '$FRAME_PATH' GetExtents 0)\""
    fi
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -20 "$RUNTIME/comp.log"
    echo "--- app log (tail) ---"; tail -20 "$RUNTIME/app.log"
    echo "==> AT-SPI test FAILED"
    exit 1
fi
echo "==> AT-SPI test PASSED"
