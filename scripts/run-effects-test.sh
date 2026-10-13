#!/usr/bin/env bash
# Headless test for R4.7 effects (docs/11-r4-implementation-plan.md R4.7): an
# effect change in WindowServer.ini [Effects] applies to the running compositor.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-effects.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT
export SDE_STATE_DIR="$RUNTIME"

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

SOCK="sde-eff-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.3
LOG="$RUNTIME/comp.log"

echo "=== (A) defaults ==="
check "geometry on (show-geometry 'OnMoveAndResize', tile-window 'ShowTileOverlay')" "$(cat "$LOG")"

echo "=== (B) an effect toggles live ==="
cat > "$RUNTIME/WindowServer.ini" <<'EOF'
[Effects]
ShowGeometry=Never
TileWindow=Never
WindowGeometry=false
EOF
for _ in $(seq 1 40); do
    grep -qF "geometry off (show-geometry 'Never', tile-window 'Never')" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "geometry off (show-geometry 'Never', tile-window 'Never')" "$(cat "$LOG")"

echo "=== (C) an effect toggles back ==="
cat > "$RUNTIME/WindowServer.ini" <<'EOF'
[Effects]
ShowGeometry=OnResizeOnly
TileWindow=ShowTileOverlay
WindowGeometry=true
EOF
for _ in $(seq 1 40); do
    grep -qF "geometry on (show-geometry 'OnResizeOnly', tile-window 'ShowTileOverlay')" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "geometry on (show-geometry 'OnResizeOnly', tile-window 'ShowTileOverlay')" "$(cat "$LOG")"

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> effects test FAILED"
    exit 1
fi
echo "==> effects test PASSED"
