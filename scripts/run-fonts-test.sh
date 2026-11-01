#!/usr/bin/env bash
# Headless test for R4.7 system fonts (docs/10-font-parity-decision.md,
# docs/11-r4-implementation-plan.md R4.7):
#   (A) a scalable family makes size-based zoom resolve (the R3 no-op is flipped);
#   (B) the session font broker ($XDG_RUNTIME_DIR/sde-fonts) applies to the
#       compositor live and persists to WindowServer.ini [Fonts];
#   (C) a persisted [Fonts] is applied at startup.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init fonts
sde_test_export_roots
# The compositor reads the font broker's file from $XDG_RUNTIME_DIR/sde-fonts:
# in the real one, this test would change a live session's fonts.
sde_test_private_runtime_dir

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

echo "=== (A) a scalable family makes zoom resolve ==="
if "$SDE_ROOT/scripts/run-font-parity-test.sh" >/dev/null; then
    echo "  ok: font-parity (scalable zoom)"
else
    echo "  FAIL: font-parity"
    SDE_TEST_FAILURES=$((SDE_TEST_FAILURES + 1))
fi

echo "=== (B) the font broker applies live and persists ==="
sde_test_case broker
sde_test_case_compositor SDE_WM_PORTAL_PATH="$CASE_RUNTIME/wm"
printf 'SerenitySans 12 400 0\nSerenitySans 12 400 0\nSerenitySans 12 400 0\n' > "$XDG_RUNTIME_DIR/sde-fonts"
sde_test_wait_log "$CASE_RUNTIME/comp.log" "applied system fonts \(default 'SerenitySans 12 400 0'\)" 40 || true
sde_test_check "$CASE_RUNTIME/comp.log" "applied system fonts \(default 'SerenitySans 12 400 0'\)" "the broker's fonts were applied live"
sde_test_check "$CASE_RUNTIME/WindowServer.ini" "^Default=SerenitySans 12 400 0" "Default persisted to [Fonts]"
sde_test_check "$CASE_RUNTIME/WindowServer.ini" "^WindowTitle=SerenitySans 12 400 0" "WindowTitle persisted to [Fonts]"
sde_test_case_stop
rm -f "$XDG_RUNTIME_DIR/sde-fonts"

echo "=== (C) a persisted [Fonts] is applied at startup ==="
sde_test_case persisted
cat > "$CASE_RUNTIME/WindowServer.ini" <<'EOF'
[Fonts]
Default=SerenitySans 14 400 0
FixedWidth=SerenitySans 14 400 0
WindowTitle=SerenitySans 14 400 0
EOF
sde_test_case_compositor SDE_WM_PORTAL_PATH="$CASE_RUNTIME/wm"
sde_test_wait_log "$CASE_RUNTIME/comp.log" "applied persisted system fonts" 40 || true
sde_test_check "$CASE_RUNTIME/comp.log" "applied persisted system fonts \(default 'SerenitySans 14 400 0'\)" "the persisted [Fonts] was applied at startup"
sde_test_case_stop

sde_test_finish
