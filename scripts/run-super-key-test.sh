#!/usr/bin/env bash
# Headless Super-key test.
#
# Super is the Taskbar's "Windows key": tapping it alone opens the Taskbar system
# menu, but it must still act as a modifier for Super+<key> shortcuts and
# Super+drag. (It used to open the menu on every Super keydown, so it never
# combined with another key.) The compositor logs "Super tapped" for the menu.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
VI="$("$SDE_ROOT/scripts/build-virtual-input.sh")"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-super.XXXXXX")"
chmod 700 "$RUNTIME"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
}
trap cleanup EXIT

cat > "$RUNTIME/WindowServer.ini" <<EOF
[Placement]
Windows=Text Editor:100,100,400,300
EOF
SOCK="sde-super-$$"
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" SDE_APP_BIN_DIR="$BUILD/bin" \
    "$COMP" -n "$SOCK" -s "$BUILD/bin/TextEditor" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 60); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 3

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
