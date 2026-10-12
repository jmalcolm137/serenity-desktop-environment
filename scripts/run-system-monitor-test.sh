#!/usr/bin/env bash
# Headless test for the Linux-native SystemMonitor (R4.9).
#
# SystemMonitor reads the host, not SerenityOS /sys/kernel files: the process
# list and CPU/memory graphs from /proc (thread stacks via libunwind + elfutils),
# the Network tab from getifaddrs()/sysfs + /proc/net/*, the Storage tab from
# /proc/mounts + statvfs(), and the per-process Open files / Memory map windows
# from /proc/<pid>/{fd,fdinfo,smaps}. This test opens every tab and a process
# window and checks none faults or fails to build its model.
#
# Runs entirely headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SYSTEM_MONITOR="$BUILD/bin/SystemMonitor"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
VIRTUAL_INPUT="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

if [[ ! -x "$SYSTEM_MONITOR" ]]; then
    echo "==> SystemMonitor not built at $SYSTEM_MONITOR"
    echo "    build it with: ninja -C $BUILD SystemMonitor"
    exit 1
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-sysmon.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$ "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$.lock 2>/dev/null || true' EXIT

SOCK="sde-sysmon-$$"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
    return 0
}
check_absent() {
    if grep -qF "$1" "$2"; then
        echo "  UNEXPECTED ($2): $1"
        fail=1
    else
        echo "  ok (absent): $1"
    fi
    return 0
}
check_no_fault() {
    if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error" "$1"; then
        echo "  FAIL: fault reported in $1"
        fail=1
    fi
    return 0
}

# Open a process window (double-click a row in the process list): this exercises
# the per-process Open files and Memory map views, built from /proc/<pid>/{fd,
# fdinfo,smaps}. The Unveiled paths tab is removed on Linux (unveil is a
# SerenityOS-only mechanism). This runs first, before any other windows have
# been mapped, so the main window is at the compositor's default (top-left).
PROC_LOG="$RUNTIME/sysmon-proc.log"
env WAYLAND_DISPLAY="$SOCK" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 25 "$SYSTEM_MONITOR" -t processes > "$PROC_LOG" 2>&1 &
SYSMON_PID=$!
sleep 3

opened=0
# Focus the window, then double-click a row in the process list.
for attempt in 1 2; do
    for y in 96 112 80 128 144; do
        for x in 120 200 60 280; do
            env WAYLAND_DISPLAY="$SOCK" "$VIRTUAL_INPUT" --extent 1280 720 \
                --move $x $y --button 0x110 press --button 0x110 release --sleep 60 \
                --button 0x110 press --button 0x110 release --sleep 450 >/dev/null 2>&1 || true
            sleep 0.5
            if grep -q "title='PID" "$PROC_LOG"; then
                opened=1
                break 3
            fi
        done
    done
    # Keyboard fallback: select the first row and activate it.
    env WAYLAND_DISPLAY="$SOCK" "$VIRTUAL_INPUT" --extent 1280 720 \
        --key 108 press --key 108 release --sleep 120 \
        --key 28 press --key 28 release --sleep 500 >/dev/null 2>&1 || true
    sleep 0.6
    if grep -q "title='PID" "$PROC_LOG"; then
        opened=1
        break
    fi
done

echo "--- SystemMonitor process window ---"
if [[ "$opened" -eq 1 ]]; then
    echo "  ok: process window opened (per-process views built)"
else
    echo "  MISSING: process window did not open"
    fail=1
fi
check_absent "Unable to update" "$PROC_LOG"
check_absent "Unable to invalidate" "$PROC_LOG"
check_no_fault "$PROC_LOG"

wait "$SYSMON_PID" 2>/dev/null || true

# Every tab that reads host facts. A failed source is logged by JsonArrayModel
# ("Unable to ..."), so its absence means the model was built.
for tab in processes network fs; do
    TAB_LOG="$RUNTIME/sysmon-$tab.log"
    env WAYLAND_DISPLAY="$SOCK" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        timeout 6 "$SYSTEM_MONITOR" -t "$tab" > "$TAB_LOG" 2>&1 || true
    echo "--- SystemMonitor '-t $tab' ---"
    tail -4 "$TAB_LOG"
    check_absent "Unable to update" "$TAB_LOG"
    check_absent "Unable to invalidate" "$TAB_LOG"
    check_no_fault "$TAB_LOG"
done

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- compositor log ---"
grep -iE "toplevel mapped|System Monitor" "$RUNTIME/comp.log" || true

echo "=== SystemMonitor headless smoke test ==="
check "toplevel mapped title='System Monitor'" "$RUNTIME/comp.log"

if [[ "$fail" -ne 0 ]]; then
    echo "==> SystemMonitor smoke test FAILED"
    exit 1
fi
echo "==> SystemMonitor smoke test PASSED"
