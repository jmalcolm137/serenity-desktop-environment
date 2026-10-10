#!/usr/bin/env bash
# Headless session-lock test (R4.1, docs/11-r4-implementation-plan.md): a minimal
# ext-session-lock-v1 client locks the session, covers every output, waits for the
# compositor's "locked" event, then unlocks. Asserts the compositor-side contract.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/lock-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
LOCK_XML="$WP/staging/ext-session-lock/ext-session-lock-v1.xml"
wayland-scanner client-header "$LOCK_XML" "$TOOLS_BUILD/ext-session-lock-v1-client-protocol.h"
wayland-scanner private-code  "$LOCK_XML" "$TOOLS_BUILD/ext-session-lock-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/lock-test.c" \
    "$TOOLS_BUILD/ext-session-lock-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/lock-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-lock.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-lock-$$"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 1

env WAYLAND_DISPLAY="$SOCK" timeout 15 "$TOOLS_BUILD/lock-test" --hold 600 > "$RUNTIME/lock.log" 2>&1 || true

sleep 1
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- client log ---"
cat "$RUNTIME/lock.log"
echo "--- compositor log ---"
cat "$RUNTIME/comp.log"

fail=0
check() {
    if grep -qF "$1" "$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
    return 0
}

echo "=== session-lock test ==="
check "LOCK-TEST: lock surface created" "$RUNTIME/lock.log"
check "LOCK-TEST: surface configured 1280x720" "$RUNTIME/lock.log"
check "LOCK-TEST: locked" "$RUNTIME/lock.log"
check "LOCK-TEST: unlocking" "$RUNTIME/lock.log"
check "WindowServer: session lock requested" "$RUNTIME/comp.log"
check "WindowServer: session lock surface covered an output" "$RUNTIME/comp.log"
check "WindowServer: session locked" "$RUNTIME/comp.log"
check "WindowServer: session unlocked" "$RUNTIME/comp.log"

if [[ "$fail" -ne 0 ]]; then
    echo "==> session-lock test FAILED"
    exit 1
fi
echo "==> session-lock test PASSED"
