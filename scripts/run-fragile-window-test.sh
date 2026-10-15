#!/usr/bin/env bash
# Fragile-window dismissal on a workspace switch (R3, docs/04).
#
# SerenityOS closes "fragile" windows (popups, menus, tooltips) as soon as focus
# moves — a new active window, the window switcher, or a workspace switch. SDE's
# client popups are client menus (`serenity-menu` layer surfaces), so the
# compositor asks their owner to close them at those points
# (`serenity_window_manager.dismiss_menus`).
#
# This drives a headless compositor + Taskbar: click the start button (the system
# menu opens), then switch workspace (Ctrl+Alt+Right); the menu must be closed.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
# The Taskbar must match this compositor's WM protocol.
"$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null 2>&1 || true

WIDTH="${SDE_FRAGILE_WIDTH:-1280}"
HEIGHT="${SDE_FRAGILE_HEIGHT:-720}"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-fragile.XXXXXX")"
chmod 700 "$RUNTIME"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

SOCK="sde-fragile-$$"
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" SDE_APP_BIN_DIR="$BUILD/bin" \
    "$COMP" -n "$SOCK" -s "$BUILD/bin/Taskbar" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
# Let the Taskbar map its layer surfaces.
sleep 4

# A headless seat has no input devices until the virtual pointer is hot-plugged, so
# the Taskbar binds its wl_pointer only once that capability arrives. Clicking in the
# same breath races that bind: on a slow runner the press reaches a client with no
# pointer yet and is dropped, while the release (after the bind) arrives alone. Plug a
# pointer in over the button first and wait for the Taskbar to see it.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --move 20 $((HEIGHT - 14)) --linger 200 >/dev/null 2>&1
for _ in $(seq 1 50); do
    grep -q 'pointer entered window' "$RUNTIME/comp.log" && break
    sleep 0.1
done

# 1) Click the Taskbar start button (bottom-left) -> the system menu opens.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --move 20 $((HEIGHT - 14)) --button 0x110 press --sleep 80 --button 0x110 release --linger 900 >/dev/null 2>&1
sleep 0.6
OPENED="$(grep -c 'client menu opened' "$RUNTIME/comp.log" || true)"

# 2) Switch workspace (Ctrl+Alt+Right): the fragile menu must be closed.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --key 29 press --sleep 60 --key 56 press --sleep 60 \
    --key 106 press --sleep 60 --key 106 release --sleep 60 \
    --key 56 release --sleep 60 --key 29 release --linger 900 >/dev/null 2>&1
sleep 0.8

DISMISSED="$(grep -c 'closed fragile windows (workspace switch)' "$RUNTIME/comp.log" || true)"
CLOSED="$(grep -c 'client menu closed' "$RUNTIME/comp.log" || true)"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== fragile windows ==="
grep -E "client menu opened|closed fragile windows|client menu closed" "$RUNTIME/comp.log" || true
check "clicking the start button opens a Taskbar menu" "[[ '$OPENED' -ge 1 ]]"
check "a workspace switch closes fragile windows" "[[ '$DISMISSED' -ge 1 ]]"
check "the menu is actually closed" "[[ '$CLOSED' -ge 1 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -25 "$RUNTIME/comp.log"
    echo "==> fragile-window test FAILED"
    exit 1
fi
echo "==> fragile-window test PASSED"
