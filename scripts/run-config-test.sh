#!/usr/bin/env bash
# Headless test for the R4.4 compositor config (docs/14-r4.4-config.md):
# defaults, get/set, validation (invalid rejected with a clear error), and live
# reload taking effect in a running compositor.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
CONFIG="$SDE_ROOT/scripts/sde-config"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-config.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT
export SDE_STATE_DIR="$RUNTIME"
sde_config() { SDE_STATE_DIR="$RUNTIME" "$CONFIG" "$@"; }

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

echo "=== (A) defaults ==="
defaults="$(sde_config list)"
check "Rows=2" "$defaults"
check "EdgeThreshold=16" "$defaults"
check "FollowsMouse=false" "$defaults"
check "WindowGeometry=true" "$defaults"
check "DefaultScale=0" "$defaults"
check "Renderer=auto" "$defaults"
check "Level=error" "$defaults"
check "SwitchWorkspaceRight=Ctrl+Alt+Right" "$defaults"
check "Lock=Super+L" "$defaults"

echo "=== (B) set / get ==="
sde_config set Focus FollowsMouse true >/dev/null
sde_config set Tiling EdgeThreshold 120 >/dev/null
if [ "$(sde_config get Focus FollowsMouse)" = "true" ]; then
    echo "  ok: get FollowsMouse=true"
else
    echo "  FAIL: get FollowsMouse (got '$(sde_config get Focus FollowsMouse)')"
    fail=1
fi
check "EdgeThreshold=120" "$(sde_config list)"
sde_config set Keybindings SwitchWorkspaceRight Ctrl+Alt+Page_Down >/dev/null
if [ "$(sde_config get Keybindings SwitchWorkspaceRight)" = "Ctrl+Alt+Page_Down" ]; then
    echo "  ok: rebind SwitchWorkspaceRight=Ctrl+Alt+Page_Down"
else
    echo "  FAIL: rebind (got '$(sde_config get Keybindings SwitchWorkspaceRight)')"
    fail=1
fi
sde_config set Keybindings SwitchWorkspaceRight Ctrl+Alt+Right >/dev/null

echo "=== (C) validation ==="
if sde_config validate >/dev/null 2>&1; then
    echo "  ok: valid config passes"
else
    echo "  FAIL: valid config rejected"
    fail=1
fi
sde_config set Workspaces Rows 99 >/dev/null
if sde_config validate >"$RUNTIME/invalid.log" 2>&1; then
    echo "  FAIL: invalid config accepted"
    fail=1
else
    echo "  ok: invalid config rejected"
    check "Rows='99' invalid (expected 1-16)" "$(cat "$RUNTIME/invalid.log")"
fi
sde_config set Workspaces Rows 2 >/dev/null  # back to a valid grid
sde_config set Keybindings SwitchWorkspaceRight Ctrl+Alt+Bogus >/dev/null
if sde_config validate >"$RUNTIME/invalid-kb.log" 2>&1; then
    echo "  FAIL: invalid keybinding accepted"
    fail=1
else
    echo "  ok: invalid keybinding rejected"
    check "SwitchWorkspaceRight='Ctrl+Alt+Bogus' invalid" "$(cat "$RUNTIME/invalid-kb.log")"
fi
sde_config set Keybindings SwitchWorkspaceRight Ctrl+Alt+Right >/dev/null

# R6/N5, R6/N8: the renderer and log level are validated too.
sde_config set Output Renderer bogus >/dev/null
if sde_config validate >"$RUNTIME/invalid-renderer.log" 2>&1; then
    echo "  FAIL: invalid renderer accepted"
    fail=1
else
    echo "  ok: invalid renderer rejected"
    check "Renderer='bogus' invalid (expected auto|gles2|vulkan|pixman)" "$(cat "$RUNTIME/invalid-renderer.log")"
fi
sde_config set Output Renderer auto >/dev/null
sde_config set Log Level bogus >/dev/null
if sde_config validate >"$RUNTIME/invalid-loglevel.log" 2>&1; then
    echo "  FAIL: invalid log level accepted"
    fail=1
else
    echo "  ok: invalid log level rejected"
    check "Level='bogus' invalid (expected error|warn|info|debug)" "$(cat "$RUNTIME/invalid-loglevel.log")"
fi
sde_config set Log Level error >/dev/null

echo "=== (D) live reload ==="
sock="sde-config-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$sock" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
# Wait for the socket, which is created after initialize() (incl. Config::watch()).
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock" ]] && break
    sleep 0.1
done
check "config: workspaces 2x2" "$(cat "$RUNTIME/comp.log")"

sde_config set Workspaces Rows 4 >/dev/null
for _ in $(seq 1 60); do
    grep -qF "workspaces 4x2" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
check "workspaces 4x2" "$(cat "$RUNTIME/comp.log")"

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "=== (E) log level: SDE_LOG_LEVEL overrides the config (R6/N8) ==="
sock_log="sde-config-log-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_LOG_LEVEL=info \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm-log" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$sock_log" > "$RUNTIME/log-level.log" 2>&1 &
LOG_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$sock_log" ]] && break
    sleep 0.1
done
pkill -P "$LOG_PID" 2>/dev/null || true
kill "$LOG_PID" 2>/dev/null || true
wait "$LOG_PID" 2>/dev/null || true
check "log level 'info'" "$(cat "$RUNTIME/log-level.log")"

if [[ "$fail" -ne 0 ]]; then
    echo "==> config test FAILED"
    exit 1
fi
echo "==> config test PASSED"
