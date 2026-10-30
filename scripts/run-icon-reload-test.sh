#!/usr/bin/env bash
# ci-tier: full
# Headless icon-reload hotkey test (R3).
#
# SerenityOS binds Ctrl+Super+Shift+I to
# WindowManager::reload_icon_bitmaps_after_scale_change(): it re-reads the config
# and re-renders the (output-scale-dependent) chrome bitmaps — window frames, which
# carry the titlebar icon — and re-reports the window icons so the Taskbar
# refreshes its buttons.
#
# This runs the compositor headless with a window and presses the hotkey, asserting
# the compositor logged the reload, and that the window is still intact afterwards.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init icon-reload
sde_test_export_roots
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

WIDTH="${SDE_ICON_RELOAD_WIDTH:-800}"
HEIGHT="${SDE_ICON_RELOAD_HEIGHT:-600}"

chmod 700 "$RUNTIME"
APP_PID=""
cleanup() {
    [[ -n "$APP_PID" ]] && kill "$APP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}

sde_test_compositor SDE_STATE_DIR="$RUNTIME"

# A window with a frame (server-side decorations) and an icon.
env WAYLAND_DISPLAY="$SOCK" "$SDE_ROOT/build/tools/native-app-test" \
    --server-side-decorations --title "Reloaded" --linger 3000 >/dev/null 2>&1 &
APP_PID=$!
for _ in $(seq 1 60); do
    grep -q "toplevel mapped title='Reloaded'" "$RUNTIME/comp.log" && break
    sleep 0.1
done
sleep 0.3

# Ctrl+Super+Shift+I.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --key 29 press --sleep 60 --key 125 press --sleep 60 --key 42 press --sleep 60 \
    --key 23 press --sleep 60 --key 23 release --sleep 60 \
    --key 42 release --sleep 60 --key 125 release --sleep 60 --key 29 release --linger 500 >/dev/null 2>&1
sleep 0.5

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== Ctrl+Super+Shift+I (reload icon bitmaps) ==="
grep "reloaded icon bitmaps" "$RUNTIME/comp.log" || true
check "the hotkey reloads the icon/frame bitmaps" "[[ \"\$(grep -c 'reloaded icon bitmaps (1 toplevels' \"\$RUNTIME/comp.log\")\" -ge 1 ]]"
# The reload must not disturb the window (it is re-rendered, not destroyed).
check "the window is still mapped" "[[ \"\$(grep -c \"toplevel destroyed\" \"\$RUNTIME/comp.log\")\" -eq 0 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -15 "$RUNTIME/comp.log"
    echo "==> icon-reload test FAILED"
    exit 1
fi
echo "==> icon-reload test PASSED"
