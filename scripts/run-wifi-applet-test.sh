#!/usr/bin/env bash
# Headless WifiApplet test.
#
# The applet fronts NetworkManager over the system D-Bus, so a real radio would
# make the result non-deterministic. Instead we start a private D-Bus with the
# mock NetworkManager (Tools/mock-networkmanager.c), point the applet's system
# bus at it via DBUS_SYSTEM_BUS_ADDRESS, and run the compositor headless.
#
# It asserts:
#   * the applet reads the mock's networks and the active connection;
#   * it maps as the "serenity-applet:WiFi" layer surface;
#   * a passphrase typed into the popup reaches NetworkManager, intact;
#   * the Taskbar's applet area places Wi-Fi immediately to the left of Battery.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
WIFI_APPLET="$SDE_ROOT/build/WifiApplet/WifiApplet"
BATTERY_APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"
MOCK="$SDE_ROOT/build/tools/mock-networkmanager"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-wifi-applet.sh" >/dev/null

if [[ ! -x "$BATTERY_APPLET" ]]; then
    "$SDE_ROOT/scripts/build-battery-applet.sh" >/dev/null 2>&1 || true
fi

mkdir -p "$SDE_ROOT/build/tools"
cc -O2 -Wall "$SDE_ROOT/Tools/mock-networkmanager.c" \
    $(pkg-config --cflags --libs dbus-1) \
    -o "$MOCK"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-wifi.XXXXXX")"
chmod 700 "$RUNTIME"

BUS_PID=""
MOCK_PID=""
COMP_PID=""
cleanup() {
    # The compositor spawns the applets (its -s shell); killing its process group
    # takes them down too, so nothing is orphaned.
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$MOCK_PID" ]] && kill "$MOCK_PID" 2>/dev/null || true
    [[ -n "$BUS_PID" ]] && kill "$BUS_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

# A private bus, used as the applet's system bus.
BUS_INFO="$(dbus-daemon --session --fork --print-address=1 --print-pid=1)"
DBUS_SYSTEM_BUS_ADDRESS="$(sed -n 1p <<<"$BUS_INFO")"
BUS_PID="$(sed -n 2p <<<"$BUS_INFO")"
export DBUS_SYSTEM_BUS_ADDRESS

"$MOCK" > "$RUNTIME/mock.log" 2>&1 &
MOCK_PID=$!
sleep 0.5

SOCK="sde-wifi-$$"
STARTUP="$WIFI_APPLET"
[[ -x "$BATTERY_APPLET" ]] && STARTUP="$STARTUP & $BATTERY_APPLET"

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_OUTPUT_SCALE=1 \
    WIFI_APPLET_LOG_LAYOUT=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" -s "$STARTUP" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 3

# --- drive the popup: connect to a secured network by typing a passphrase -----
# Open the applet, click the secured, unsaved OpenGuest network (it opens in
# place with a passphrase field, which takes the focus), type a passphrase and
# press Enter. This exercises the compositor's on_demand layer keyboard focus
# (without it, the field takes no input) and the applet's
# AddAndActivateConnection path; the mock records the passphrase it received.
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"
# Wait for the applet area to settle (Wi-Fi shifts left once Battery maps).
LAYOUT=""
for _ in $(seq 1 60); do
    LAYOUT="$(grep -oE "'Battery' [0-9]+x[0-9]+\+[0-9]+,[0-9]+; 'WiFi' [0-9]+x[0-9]+\+[0-9]+,[0-9]+" "$RUNTIME/comp.log" | tail -1)"
    [[ -n "$LAYOUT" ]] && break
    sleep 0.1
done
APPLET_X="$(sed -nE "s/.*'WiFi' [0-9]+x[0-9]+\+([0-9]+),.*/\1/p" <<<"$LAYOUT")"
APPLET_Y="$(sed -nE "s/.*'WiFi' [0-9]+x[0-9]+\+[0-9]+,([0-9]+).*/\1/p" <<<"$LAYOUT")"
VI_PID=""
if [[ -n "$APPLET_X" && -n "$APPLET_Y" ]]; then
    # The first virtual device opens the popup and stays connected: a keyboard
    # that appears after the popup took the focus is a different case from the
    # one under test here.
    WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
        --move $((APPLET_X + 8)) $((APPLET_Y + 8)) --sleep 400 --button 0x110 press --sleep 150 --button 0x110 release \
        --linger 15000 >/dev/null 2>&1 &
    VI_PID=$!
    # Where the popup put OpenGuest (it logs its rows; opening it scans, which
    # can add a row, so wait for the list to stop changing).
    ROW=""
    for _ in $(seq 1 50); do
        sleep 0.2
        NEXT="$(grep -oE "layout row 'OpenGuest' [0-9]+x[0-9]+\+[0-9]+,[0-9]+" "$RUNTIME/comp.log" | tail -1 || true)"
        [[ -n "$NEXT" && "$NEXT" == "$ROW" ]] && break
        ROW="$NEXT"
    done
    ROW_X="$(sed -nE 's/.*\+([0-9]+),([0-9]+)$/\1/p' <<<"$ROW")"
    ROW_Y="$(sed -nE 's/.*\+([0-9]+),([0-9]+)$/\2/p' <<<"$ROW")"
    if [[ -n "$ROW_X" && -n "$ROW_Y" ]]; then
        # "secret" then Enter.
        WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 \
            --move $((ROW_X + 60)) $((ROW_Y + 12)) --sleep 400 --button 0x110 press --sleep 150 --button 0x110 release --sleep 1200 \
            --key 31 press --key 31 release --sleep 50 --key 18 press --key 18 release --sleep 50 \
            --key 46 press --key 46 release --sleep 50 --key 19 press --key 19 release --sleep 50 \
            --key 18 press --key 18 release --sleep 50 --key 20 press --key 20 release --sleep 50 \
            --key 28 press --key 28 release --sleep 600 --linger 600 >/dev/null 2>&1 || true
    fi
    kill "$VI_PID" 2>/dev/null || true
    wait "$VI_PID" 2>/dev/null || true
fi

kill -TERM -- "-$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true
COMP_PID=""

fail=0
check() {
    local file="${2:-$RUNTIME/comp.log}"
    if grep -qF "$1" "$file"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

echo "--- Wi-Fi applet ---"
grep -E "WifiApplet|serenity-applet:WiFi|layout 'WiFi'" "$RUNTIME/comp.log" || true

echo "=== WifiApplet ==="
check "WifiApplet: 2 networks, connected to MockNet (82%)"
check "layer surface namespace='serenity-applet:WiFi' layer=top"

# The passphrase reached NetworkManager: keyboard input made it into the popup's
# on_demand layer surface, through the applet, to AddAndActivateConnection.
check "mock-networkmanager: org.freedesktop.NetworkManager.AddAndActivateConnection" "$RUNTIME/mock.log"
check "mock-networkmanager: add psk=secret" "$RUNTIME/mock.log"

# The applet area must place Wi-Fi immediately to the left of Battery.
LAYOUT="$(grep -oE "'Battery' [0-9]+x[0-9]+\+[0-9]+,[0-9]+; 'WiFi' [0-9]+x[0-9]+\+[0-9]+,[0-9]+" "$RUNTIME/comp.log" | tail -1)"
BATTERY_X="$(sed -nE "s/.*'Battery' [0-9]+x[0-9]+\+([0-9]+),.*/\1/p" <<<"$LAYOUT")"
WIFI_X="$(sed -nE "s/.*'WiFi' [0-9]+x[0-9]+\+([0-9]+),.*/\1/p" <<<"$LAYOUT")"
if [[ -n "$BATTERY_X" && -n "$WIFI_X" && "$WIFI_X" -lt "$BATTERY_X" ]]; then
    echo "  ok: Wi-Fi (x=$WIFI_X) is laid out left of the battery applet (x=$BATTERY_X)"
else
    echo "  MISSING: Wi-Fi left of battery ($LAYOUT)"
    fail=1
fi

if grep -q "VERIFICATION FAILED" "$RUNTIME/comp.log"; then
    echo "  FAIL: the applet aborted"
    grep -B2 -A4 "VERIFICATION FAILED" "$RUNTIME/comp.log" | head -20
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -40 "$RUNTIME/comp.log"
    echo "==> WifiApplet test FAILED"
    exit 1
fi
echo "==> WifiApplet test PASSED"
