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
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"

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

echo "=== (A) a scalable family makes zoom resolve ==="
"$SDE_ROOT/scripts/run-font-parity-test.sh" >/dev/null && echo "  ok: font-parity (scalable zoom)" || { echo "  FAIL: font-parity"; fail=1; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-fonts.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

start_compositor() {
    local sock="$1"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        XDG_RUNTIME_DIR="$RUNTIME" \
        SDE_STATE_DIR="$RUNTIME" \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm-$sock" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" > "$RUNTIME/comp-$sock.log" 2>&1 &
    echo $!
    for _ in $(seq 1 80); do
        [[ -e "$RUNTIME/$sock" ]] && break
        sleep 0.1
    done
    sleep 0.3
}

echo "=== (B) the font broker applies live and persists ==="
COMP_PID="$(start_compositor sde-font-a)"
LOG="$RUNTIME/comp-sde-font-a.log"
printf 'SerenitySans 12 400 0\nSerenitySans 12 400 0\nSerenitySans 12 400 0\n' > "$RUNTIME/sde-fonts"
for _ in $(seq 1 40); do
    grep -qF "applied system fonts (default 'SerenitySans 12 400 0')" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "applied system fonts (default 'SerenitySans 12 400 0')" "$(cat "$LOG")"
check "Default=SerenitySans 12 400 0" "$(cat "$RUNTIME/WindowServer.ini" 2>/dev/null)"
check "WindowTitle=SerenitySans 12 400 0" "$(cat "$RUNTIME/WindowServer.ini" 2>/dev/null)"
pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "=== (C) a persisted [Fonts] is applied at startup ==="
cat > "$RUNTIME/WindowServer.ini" <<'EOF'
[Fonts]
Default=SerenitySans 14 400 0
FixedWidth=SerenitySans 14 400 0
WindowTitle=SerenitySans 14 400 0
EOF
COMP_PID="$(start_compositor sde-font-b)"
LOG="$RUNTIME/comp-sde-font-b.log"
check "applied persisted system fonts (default 'SerenitySans 14 400 0')" "$(cat "$LOG")"
pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> fonts test FAILED"
    exit 1
fi
echo "==> fonts test PASSED"
