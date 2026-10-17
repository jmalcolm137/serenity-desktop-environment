#!/usr/bin/env bash
# Headless test for R4.9 theme config (docs/11-r4-implementation-plan.md): the
# chosen system theme is persisted to WindowServer.ini [Theme] and applied at
# startup, and a live theme change from the broker is applied and persisted.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
DARK="$SERENITY/Base/res/themes/Dark.ini"
REDMOND="$SERENITY/Base/res/themes/Redmond 2000.ini"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-themes.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}

start_compositor() {
    local sock="$1"
    local startup="${2:-}"
    local -a startup_args=()
    [[ -n "$startup" ]] && startup_args=(-s "$startup")
    env -u WAYLAND_DISPLAY -u DISPLAY \
        WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        XDG_RUNTIME_DIR="$RUNTIME" \
        SDE_STATE_DIR="$RUNTIME" \
        SDE_WM_PORTAL_PATH="$RUNTIME/wm-$sock" \
        SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
        LD_LIBRARY_PATH="$BUILD/lib" \
        "$COMP" -n "$sock" "${startup_args[@]}" > "$RUNTIME/comp-$sock.log" 2>&1 &
    echo $!
    for _ in $(seq 1 80); do
        [[ -e "$RUNTIME/$sock" ]] && break
        sleep 0.1
    done
    sleep 0.4
}

echo "=== (A) a persisted [Theme] is applied at startup ==="
cat > "$RUNTIME/WindowServer.ini" <<EOF
[Theme]
Name=Dark
Path=$DARK
EOF
COMP_PID="$(start_compositor sde-theme-a)"
LOG="$RUNTIME/comp-sde-theme-a.log"
check "applied system theme '$DARK'" "$(cat "$LOG")"
pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "=== (B) a live theme change is applied and persisted ==="
# The Taskbar's Themes menu is a client of the same broker/host-settings channel.
# With it running, check that a client hears the *new* theme name: the broker only
# carries the path, so the name comes from the host-settings re-broadcast. Without
# that, a theme menu keeps highlighting the previously selected theme.
STARTUP=""
[[ -x "$BUILD/bin/Taskbar" ]] && STARTUP="$BUILD/bin/Taskbar"
COMP_PID="$(start_compositor sde-theme-b "$STARTUP")"
LOG="$RUNTIME/comp-sde-theme-b.log"
printf '%s\n' "$REDMOND" > "$RUNTIME/sde-theme"
for _ in $(seq 1 40); do
    grep -qF "applied system theme '$REDMOND'" "$LOG" 2>/dev/null && break
    sleep 0.1
done
check "applied system theme '$REDMOND'" "$(cat "$LOG")"
check "Name=Redmond 2000" "$(cat "$RUNTIME/WindowServer.ini")"
check "Path=$REDMOND" "$(cat "$RUNTIME/WindowServer.ini")"
if [[ -n "$STARTUP" ]]; then
    for _ in $(seq 1 40); do
        grep -qF "LibWM: system theme is 'Redmond 2000'" "$LOG" 2>/dev/null && break
        sleep 0.1
    done
    check "LibWM: system theme is 'Redmond 2000'" "$(cat "$LOG")"
fi
pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "=== (C) the persisted change is applied on the next start ==="
COMP_PID="$(start_compositor sde-theme-c)"
LOG="$RUNTIME/comp-sde-theme-c.log"
check "applied system theme '$REDMOND'" "$(cat "$LOG")"
pkill -P "$COMP_PID" 2>/dev/null || true
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> themes test FAILED"
    exit 1
fi
echo "==> themes test PASSED"
