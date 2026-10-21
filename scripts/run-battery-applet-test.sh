#!/usr/bin/env bash
# Headless BatteryApplet test.
#
# The applet reads the Linux power-supply class (sysfs). To make the result
# deterministic we point it at a fixture directory with --power-supply-root. The
# compositor runs headless and spawns the applet, so this is CI-friendly.
#
# It asserts:
#   * the reader reports the fixture battery (capacity and state), and the
#     "no battery" case when the directory is empty;
#   * the applet maps as the "serenity-applet:Battery" layer surface;
#   * the compositor places it in the Taskbar's applet area (the fallback
#     bottom-right of the usable area, since no Taskbar runs here).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
APPLET="$SDE_ROOT/build/BatteryApplet/BatteryApplet"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-battery-applet.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-battery.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}
check_absent() {
    if grep -qF "$1" "$2"; then
        echo "  UNEXPECTED ($2): $1"
        fail=1
    else
        echo "  ok (absent): $1"
    fi
}

# Runs the applet against `fixture` and prints the compositor log path.
run_case() {
    local fixture="$1"
    local log="$2"
    local sock="sde-battery-$$-$RANDOM"

    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless \
        WLR_HEADLESS_OUTPUTS=1 \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" -s "$APPLET --power-supply-root $fixture" > "$log" 2>&1 &
    local comp_pid=$!

    for _ in $(seq 1 50); do
        [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
        sleep 0.1
    done
    sleep 3

    pkill -P "$comp_pid" 2>/dev/null || true
    kill "$comp_pid" 2>/dev/null || true
    wait "$comp_pid" 2>/dev/null || true
}

# A charging battery: 42% (2.1 Ah of 5.0 Ah), 5:48 until full at 0.5 A.
FIXTURE="$RUNTIME/power_supply"
mkdir -p "$FIXTURE/BAT0"
echo Battery > "$FIXTURE/BAT0/type"
echo 1 > "$FIXTURE/BAT0/present"
echo 42 > "$FIXTURE/BAT0/capacity"
echo Charging > "$FIXTURE/BAT0/status"
echo TestBattery > "$FIXTURE/BAT0/model_name"
echo 2100000 > "$FIXTURE/BAT0/charge_now"
echo 5000000 > "$FIXTURE/BAT0/charge_full"
echo 500000 > "$FIXTURE/BAT0/current_now"

run_case "$FIXTURE" "$RUNTIME/charging.log"
echo "--- charging fixture ---"
grep -E "BatteryApplet|serenity-applet" "$RUNTIME/charging.log" || true
echo "=== BatteryApplet: charging fixture ==="
check "BatteryApplet: 1 batteries, 42%, Charging" "$RUNTIME/charging.log"
check "layer surface namespace='serenity-applet:Battery' layer=top" "$RUNTIME/charging.log"
check "AppletManager: applet area size 50x19 (1 applets: Battery,)" "$RUNTIME/charging.log"

# A worn battery that reports Full. Some drivers derive `capacity` from the
# design capacity (79% here), but the state of charge is 100%.
FULL="$RUNTIME/full"
mkdir -p "$FULL/BAT0"
echo Battery > "$FULL/BAT0/type"
echo 1 > "$FULL/BAT0/present"
echo 79 > "$FULL/BAT0/capacity"
echo Full > "$FULL/BAT0/status"
echo TestBattery > "$FULL/BAT0/model_name"
echo 3427000 > "$FULL/BAT0/charge_now"
echo 3412000 > "$FULL/BAT0/charge_full"
run_case "$FULL" "$RUNTIME/full.log"
echo "--- full fixture ---"
grep -E "BatteryApplet|serenity-applet" "$RUNTIME/full.log" || true
echo "=== BatteryApplet: full battery ==="
check "BatteryApplet: 1 batteries, 100%, Fully charged" "$RUNTIME/full.log"

# An empty directory: no battery.
EMPTY="$RUNTIME/empty"
mkdir -p "$EMPTY"
run_case "$EMPTY" "$RUNTIME/empty.log"
echo "--- empty fixture ---"
grep -E "BatteryApplet|serenity-applet" "$RUNTIME/empty.log" || true
echo "=== BatteryApplet: no battery ==="
check "BatteryApplet: no battery" "$RUNTIME/empty.log"
check "layer surface namespace='serenity-applet:Battery' layer=top" "$RUNTIME/empty.log"

# No power-supply class at all (a plug-in desktop, a container): the applet must
# treat that as mains power, say so once rather than every poll, and still map.
ABSENT="$RUNTIME/absent"   # deliberately never created
run_case "$ABSENT" "$RUNTIME/absent.log"
echo "--- absent power-supply class ---"
grep -E "BatteryApplet|serenity-applet" "$RUNTIME/absent.log" || true
echo "=== BatteryApplet: no power-supply class ==="
check "no power-supply class at" "$RUNTIME/absent.log"
# Once, not once per poll: the message used to be logged on every 5s refresh.
occurrences="$(grep -c "no power-supply class at" "$RUNTIME/absent.log" 2>/dev/null || echo 0)"
if [[ "$occurrences" -le 1 ]]; then
    echo "  ok: the missing power-supply class is reported once (not per poll)"
else
    echo "  MISSING: reported $occurrences times"
    fail=1
fi
check "layer surface namespace='serenity-applet:Battery' layer=top" "$RUNTIME/absent.log"
# The old wording read as a fault; a desktop on mains is not one.
if grep -q "unable to read the power-supply class" "$RUNTIME/absent.log"; then
    echo "  MISSING: still reports the desktop case as an error"
    fail=1
else
    echo "  ok: the desktop case is not reported as an error"
fi

# An applet may start while the display is blanked (DPMS off). With no enabled
# output the compositor clears the primary output; it used to refuse the layer
# surface and the client died with "unknown wl_surface". It must instead accept
# the surface and configure it when the output comes back.
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
SDE_DPMS="$SDE_ROOT/build/tools/sde-dpms"
DPMS_SOCK="sde-battery-dpms-$$-$RANDOM"
DPMS_LOG="$RUNTIME/dpms.log"
DPMS_APPLET_LOG="$RUNTIME/dpms-applet.log"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$DPMS_SOCK" > "$DPMS_LOG" 2>&1 &
dpms_comp_pid=$!
for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$DPMS_SOCK" ]] && break
    sleep 0.1
done
env WAYLAND_DISPLAY="$DPMS_SOCK" "$SDE_DPMS" off >/dev/null 2>&1 || true
sleep 0.5
env WAYLAND_DISPLAY="$DPMS_SOCK" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$APPLET" --power-supply-root "$FIXTURE" > "$DPMS_APPLET_LOG" 2>&1 &
dpms_applet_pid=$!
sleep 2
if kill -0 "$dpms_applet_pid" 2>/dev/null; then
    echo "  ok: the applet stays up while the display is off"
else
    echo "  MISSING: the applet died while the display was off"
    fail=1
fi
env WAYLAND_DISPLAY="$DPMS_SOCK" "$SDE_DPMS" on >/dev/null 2>&1 || true
sleep 2
echo "=== BatteryApplet: starting while DPMS-off ==="
check "BatteryApplet: 1 batteries, 42%, Charging" "$DPMS_APPLET_LOG"
check "layer surface namespace='serenity-applet:Battery' layer=top" "$DPMS_LOG"
check "layer configure 50x16" "$DPMS_APPLET_LOG"
check_absent "unknown wl_surface" "$DPMS_APPLET_LOG"
kill "$dpms_applet_pid" 2>/dev/null || true
wait "$dpms_applet_pid" 2>/dev/null || true
pkill -P "$dpms_comp_pid" 2>/dev/null || true
kill "$dpms_comp_pid" 2>/dev/null || true
wait "$dpms_comp_pid" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> BatteryApplet test FAILED"
    exit 1
fi
echo "==> BatteryApplet test PASSED"
