#!/usr/bin/env bash
# Headless test for R4.7 effects (docs/11-r4-implementation-plan.md R4.7): an
# effect change in WindowServer.ini [Effects] applies to the running compositor.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init effects
sde_test_export_roots

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null


fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

sde_test_compositor SDE_STATE_DIR="$RUNTIME" SDE_WM_PORTAL_PATH="$RUNTIME/wm"
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
