#!/usr/bin/env bash
# ci-tier: fast
# High contrast (R5 accessibility, docs/29-accessibility.md).
#
# [Accessibility] HighContrast forces the Contrast palette: the compositor applies it
# to its own chrome and publishes it to the theme broker so every client follows, and
# a later theme change cannot turn the mode off.
#
# Headless: the assertions are the compositor's log and the broker file it writes.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init high-contrast
sde_test_export_roots
# The compositor publishes the applied theme in $XDG_RUNTIME_DIR/sde-theme, the
# file a live session's clients follow.
sde_test_private_runtime_dir

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

start_compositor() {
    local name="$1" high_contrast="$2"
    sde_test_case "$name"
    {
        echo "[Accessibility]"
        echo "HighContrast=$high_contrast"
    } > "$CASE_RUNTIME/WindowServer.ini"
    rm -f "$XDG_RUNTIME_DIR/sde-theme"
    sde_test_case_compositor SDE_XWAYLAND=0
    sleep 2
}

BROKER="$XDG_RUNTIME_DIR/sde-theme"

# --- off: the mode is inert -------------------------------------------------
start_compositor off false
OFF_LOG="$CASE_RUNTIME/comp.log"
OFF_BROKER="$(cat "$BROKER" 2>/dev/null || true)"
sde_test_case_stop

echo "=== high contrast off ==="
sde_test_check "$OFF_LOG" "high contrast off" "the mode is off in the config log"
sde_test_refute "$OFF_LOG" "high contrast on" "the compositor did not force a theme"
sde_test_expect "the broker was not set to Contrast" test "${OFF_BROKER/Contrast.ini/}" = "$OFF_BROKER"

# --- on: forced, published, and not overridable ------------------------------
start_compositor on true
ON_LOG="$CASE_RUNTIME/comp.log"
CONTRAST="$SERENITY/Base/res/themes/Contrast.ini"

echo "=== high contrast on ==="
grep -aE "high contrast" "$ON_LOG" | head -4 || true
sde_test_check "$ON_LOG" "high contrast on; forced '$CONTRAST'" "the compositor forced the Contrast theme"
sde_test_check "$BROKER" "Contrast\.ini" "the broker names the Contrast theme (clients follow)"

# A theme change while the mode is on must not take effect, and the broker is put
# back to what is actually applied.
printf '%s\n' "$SERENITY/Base/res/themes/Dark.ini" > "$BROKER"
sleep 1.5
sde_test_check "$ON_LOG" "ignoring the theme broker" "a theme change is ignored while the mode is on"
sde_test_check "$BROKER" "Contrast\.ini" "the broker is put back to the applied theme"
sde_test_refute "$BROKER" "Dark\.ini" "and no longer names the ignored one"
sde_test_case_stop

sde_test_finish "$ON_LOG"
