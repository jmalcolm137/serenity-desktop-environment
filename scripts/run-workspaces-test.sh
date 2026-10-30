#!/usr/bin/env bash
# Headless test for R4.8 workspaces & toplevels (docs/11-r4-implementation-plan.md
# R4.8): the SDE grid is published over the standard ext-workspace-v1 protocol,
# activating a workspace switches the compositor, and a grid change applies live.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init workspaces
sde_test_export_roots
WS="$SDE_ROOT/build/tools/ext-workspace-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null


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

echo "=== (A) the default 2x2 grid over ext-workspace-v1 ==="
list="$(env WAYLAND_DISPLAY="$SOCK" "$WS" 2>&1)"
check "ext-workspace-test: 4 workspaces" "$list"
check "workspace 0 id=0,0 name=1 active=1 coords=0,0" "$list"
check "workspace 3 id=1,1 name=4 active=0 coords=1,1" "$list"

echo "=== (B) activating a workspace switches the compositor ==="
env WAYLAND_DISPLAY="$SOCK" "$WS" 4 >/dev/null 2>&1
for _ in $(seq 1 40); do
    grep -qF "switched to workspace 1,1" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "ext-workspace-v1 activate 1,1" "$(cat "$LOG")"
check "switched to workspace 1,1" "$(cat "$LOG")"

echo "=== (C) the new active state is published ==="
list2="$(env WAYLAND_DISPLAY="$SOCK" "$WS" 2>&1)"
check "workspace 3 id=1,1 name=4 active=1 coords=1,1" "$list2"

echo "=== (D) a grid change applies live ==="
cat > "$RUNTIME/WindowServer.ini" <<'EOF'
[Workspaces]
Rows=3
Columns=2
EOF
for _ in $(seq 1 40); do
    grep -qF "workspaces 3x2" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "workspaces 3x2" "$(cat "$LOG")"
list3="$(env WAYLAND_DISPLAY="$SOCK" "$WS" 2>&1)"
check "ext-workspace-test: 6 workspaces" "$list3"
check "workspace 5 id=2,1 name=6 active=0 coords=1,2" "$list3"

pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> workspaces test FAILED"
    exit 1
fi
echo "==> workspaces test PASSED"
