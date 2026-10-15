#!/usr/bin/env bash
# ci-tier: full
# Smoke test: launch a set of protocol-heavy Serenity apps against the compositor
# and assert none of them fault or print an IPC parse error.
#
# This catches a whole class of regressions: the WindowServer IPC `MessageID` is
# positional, so inserting a method mid-interface shifts every later id and breaks
# apps built against the old enum ("Failed to parse IPC message") until everything
# is rebuilt. New methods must be appended (see libgui-wayland patch 0042).
#
# Runs headless (no host compositor, no DRM), so it is CI-friendly.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
SOCK="sde-apps-$$"

# Settings apps exercise the WindowServer protocol hardest (mouse/keyboard/display
# get/set), so they surfaced the positional-id bug.
APPS=(MouseSettings KeyboardSettings DisplaySettings SystemMonitor FileManager TextEditor ThemeEditor Calculator)

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-apps.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
# The compositor spawns each app itself (the apps are separate, not -s children),
# so kill the compositor and any client it left behind.
stop_compositor() {
    kill -TERM "$COMP_PID" 2>/dev/null || true
    wait "$COMP_PID" 2>/dev/null || true
}
trap 'stop_compositor; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

fail=0
before="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
for app in "${APPS[@]}"; do
    if [[ ! -x "$BUILD/bin/$app" ]]; then
        echo "  SKIP: $app (not built)"
        continue
    fi
    log="$RUNTIME/$app.log"
    env WAYLAND_DISPLAY="$SOCK" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        SDE_APP_BIN_DIR="$BUILD/bin" \
        timeout 5 "$BUILD/bin/$app" > "$log" 2>&1 || true
    after="$(grep -c 'toplevel mapped' "$RUNTIME/comp.log" || true)"
    faults="$(grep -ciE 'Failed to parse IPC message|SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED|Runtime error' "$log" || true)"
    if [[ "$faults" -ne 0 ]]; then
        echo "  FAIL: $app: $(grep -iE 'Failed to parse IPC message|SIGSEGV|ASSERTION FAILED|VERIFICATION FAILED|Runtime error' "$log" | head -1)"
        fail=1
    elif [[ "$after" -le "$before" ]]; then
        echo "  FAIL: $app did not map a window (log: $log)"
        fail=1
    else
        echo "  ok: $app"
    fi
    before="$after"
done

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"
    tail -20 "$RUNTIME/comp.log"
    echo "==> apps smoke test FAILED"
    exit 1
fi
echo "==> apps smoke test PASSED"
