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
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

start_compositor() {
    local name="$1" high_contrast="$2"
    local runtime
    runtime="$(mktemp -d "${TMPDIR:-/tmp}/sde-contrast-$name.XXXXXX")"
    chmod 700 "$runtime"
    mkdir -p "$runtime/run"
    {
        echo "[Accessibility]"
        echo "HighContrast=$high_contrast"
    } > "$runtime/WindowServer.ini"
    setsid env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
        XDG_RUNTIME_DIR="$runtime/run" SDE_STATE_DIR="$runtime" SDE_XWAYLAND=0 \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "sde-contrast-$name-$$" > "$runtime/comp.log" 2>&1 &
    local pid=$!
    for _ in $(seq 1 80); do
        [[ -e "$runtime/run/sde-contrast-$name-$$" ]] && break
        sleep 0.1
    done
    sleep 2
    echo "$pid $runtime"
}

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

# --- off: the mode is inert -------------------------------------------------
read -r PID RUNTIME <<<"$(start_compositor off false)"
OFF_LOG="$RUNTIME/comp.log"
OFF_BROKER="$(cat "$RUNTIME/run/sde-theme" 2>/dev/null || true)"
kill -TERM -- "-$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

echo "=== high contrast off ==="
check "the mode is off in the config log" "grep -q 'high contrast off' '$OFF_LOG'"
check "the compositor did not force a theme" "! grep -q 'high contrast on' '$OFF_LOG'"
check "the broker was not set to Contrast" "[[ '$OFF_BROKER' != *Contrast.ini* ]]"

# --- on: forced, published, and not overridable ------------------------------
read -r PID RUNTIME <<<"$(start_compositor on true)"
ON_LOG="$RUNTIME/comp.log"
CONTRAST="$SERENITY/Base/res/themes/Contrast.ini"
BROKER="$RUNTIME/run/sde-theme"

echo "=== high contrast on ==="
grep -aE "high contrast" "$ON_LOG" | head -4 || true
check "the compositor forced the Contrast theme" "grep -q \"high contrast on; forced '$CONTRAST'\" '$ON_LOG'"
check "the broker names the Contrast theme (clients follow)" "grep -q 'Contrast.ini' '$BROKER'"

# A theme change while the mode is on must not take effect, and the broker is put
# back to what is actually applied.
printf '%s\n' "$SERENITY/Base/res/themes/Dark.ini" > "$BROKER"
sleep 1.5
check "a theme change is ignored while the mode is on" "grep -q 'ignoring the theme broker' '$ON_LOG'"
check "the broker is kept in agreement with the applied theme" "grep -q 'Contrast.ini' '$BROKER' && ! grep -q 'Dark.ini' '$BROKER'"

kill -TERM -- "-$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "--- high-contrast-on log (tail) ---"; tail -20 "$ON_LOG"
    echo "==> high-contrast test FAILED"
    exit 1
fi
echo "==> high-contrast test PASSED"
