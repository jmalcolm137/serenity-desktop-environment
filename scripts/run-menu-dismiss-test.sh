#!/usr/bin/env bash
# Client-menu dismissal test (R3, docs/04).
#
# The Taskbar's (and Desktop's) menus are overlay layer surfaces with no input
# grab, so wlroots never routes an outside press to them and the client cannot
# dismiss them itself — the menu looked "stuck open". The compositor instead asks
# the owner to close its menus (`serenity_window_manager.dismiss_menus`) on a
# press that lands on another client's surface.
#
# This drives a headless compositor + Taskbar: click the start button (the system
# menu opens), then click the desktop (it must be dismissed). Asserted from the
# compositor log: "client menu opened" -> "dismissing client menus on an outside
# press" + "client menu closed".
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init menu-dismiss
sde_test_export_roots
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
# The Taskbar must match this compositor's WM protocol.
"$SDE_ROOT/scripts/build-taskbar.sh" >/dev/null 2>&1 || true

WIDTH="${SDE_MENU_WIDTH:-1280}"
HEIGHT="${SDE_MENU_HEIGHT:-720}"

chmod 700 "$RUNTIME"
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}

sde_test_compositor --startup "$BUILD/bin/Taskbar" SDE_STATE_DIR="$RUNTIME"
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

# 2) Click the desktop -> the menu must be dismissed.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent "$WIDTH" "$HEIGHT" \
    --move $((WIDTH / 2)) $((HEIGHT / 2)) --button 0x110 press --sleep 80 --button 0x110 release --linger 900 >/dev/null 2>&1
sleep 0.6
DISMISSED="$(grep -c 'dismissing client menus on an outside press' "$RUNTIME/comp.log" || true)"
CLOSED="$(grep -c 'client menu closed' "$RUNTIME/comp.log" || true)"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== client menus ==="
grep -E "client menu opened|dismissing client menus|client menu closed" "$RUNTIME/comp.log" || true
check "clicking the start button opens a Taskbar menu" "[[ '$OPENED' -ge 1 ]]"
check "an outside press dismisses the menu" "[[ '$DISMISSED' -ge 1 ]]"
check "the menu is actually closed" "[[ '$CLOSED' -ge 1 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -30 "$RUNTIME/comp.log"
    echo "==> menu-dismiss test FAILED"
    exit 1
fi
echo "==> menu-dismiss test PASSED"
