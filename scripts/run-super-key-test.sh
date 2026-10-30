#!/usr/bin/env bash
# Headless Super-key test.
#
# Super is the Taskbar's "Windows key": tapping it alone opens the Taskbar system
# menu, but it must still act as a modifier for Super+<key> shortcuts and
# Super+drag. (It used to open the menu on every Super keydown, so it never
# combined with another key.) The compositor logs "Super tapped" for the menu.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init super-key
sde_test_export_roots
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

chmod 700 "$RUNTIME"
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}

cat > "$RUNTIME/WindowServer.ini" <<EOF
[Placement]
Windows=Text Editor:100,100,400,300
EOF
sde_test_compositor --startup "$BUILD/bin/TextEditor" SDE_STATE_DIR="$RUNTIME"

# 1) Tap Super alone -> the Taskbar menu.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --key 125 press --sleep 80 --key 125 release --linger 500 >/dev/null 2>&1
sleep 0.4
TAPS_AFTER_TAP="$(grep -c 'Super tapped' "$RUNTIME/comp.log" || true)"

# 2) Super+LMB drag (a shortcut, not a tap: no menu on release).
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --move 250 200 --key 125 press --sleep 60 --button 0x110 press --sleep 60 --move 300 250 --sleep 60 --button 0x110 release --sleep 60 --key 125 release --linger 500 >/dev/null 2>&1
sleep 0.4

# 3) Super+Down -> minimize.
env WAYLAND_DISPLAY="$SOCK" "$VI" --extent 1280 720 --key 125 press --sleep 60 --key 108 press --sleep 60 --key 108 release --sleep 60 --key 125 release --linger 500 >/dev/null 2>&1
sleep 0.4

TAPS="$(grep -c 'Super tapped' "$RUNTIME/comp.log" || true)"
MINIMIZE="$(grep -c 'minimize animation' "$RUNTIME/comp.log" || true)"

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  MISSING: $1"; fail=1; fi; }

echo "=== Super key ==="
grep -E "Super tapped|minimize animation" "$RUNTIME/comp.log" || true
check "tapping Super opens the Taskbar menu" "[[ '$TAPS_AFTER_TAP' -eq 1 ]]"
check "Super+Down minimizes" "[[ '$MINIMIZE' -eq 1 ]]"
check "Super combos/drag do not open the menu" "[[ '$TAPS' -eq 1 ]]"

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log (tail) ---"; tail -20 "$RUNTIME/comp.log"
    echo "==> Super-key test FAILED"
    exit 1
fi
echo "==> Super-key test PASSED"
