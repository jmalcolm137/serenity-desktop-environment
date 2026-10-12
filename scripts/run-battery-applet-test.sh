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
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$ "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$.lock 2>/dev/null || true' EXIT

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
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

# A charging battery: 42%, 8:00 until full (4.0 Ah left at 0.5 A).
FIXTURE="$RUNTIME/power_supply"
mkdir -p "$FIXTURE/BAT0"
echo Battery > "$FIXTURE/BAT0/type"
echo 1 > "$FIXTURE/BAT0/present"
echo 42 > "$FIXTURE/BAT0/capacity"
echo Charging > "$FIXTURE/BAT0/status"
echo TestBattery > "$FIXTURE/BAT0/model_name"
echo 1000000 > "$FIXTURE/BAT0/charge_now"
echo 5000000 > "$FIXTURE/BAT0/charge_full"
echo 500000 > "$FIXTURE/BAT0/current_now"

run_case "$FIXTURE" "$RUNTIME/charging.log"
echo "--- charging fixture ---"
grep -E "BatteryApplet|serenity-applet" "$RUNTIME/charging.log" || true
echo "=== BatteryApplet: charging fixture ==="
check "BatteryApplet: 1 batteries, 42%, Charging" "$RUNTIME/charging.log"
check "layer surface namespace='serenity-applet:Battery' layer=top" "$RUNTIME/charging.log"
check "AppletManager: applet area size 50x19 (1 applets: Battery,)" "$RUNTIME/charging.log"

# An empty directory: no battery.
EMPTY="$RUNTIME/empty"
mkdir -p "$EMPTY"
run_case "$EMPTY" "$RUNTIME/empty.log"
echo "--- empty fixture ---"
grep -E "BatteryApplet|serenity-applet" "$RUNTIME/empty.log" || true
echo "=== BatteryApplet: no battery ==="
check "BatteryApplet: no battery" "$RUNTIME/empty.log"
check "layer surface namespace='serenity-applet:Battery' layer=top" "$RUNTIME/empty.log"

if [[ "$fail" -ne 0 ]]; then
    echo "==> BatteryApplet test FAILED"
    exit 1
fi
echo "==> BatteryApplet test PASSED"
