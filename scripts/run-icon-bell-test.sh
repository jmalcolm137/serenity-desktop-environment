#!/usr/bin/env bash
# Functional test for the standard-protocol parity additions (docs/21):
# xdg-toplevel-icon-v1 (a client sets a window icon) and xdg-system-bell-v1 (a
# client rings the system bell). Runs headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

TOOLS_BUILD="$SDE_ROOT/build/icon-bell-test"
mkdir -p "$TOOLS_BUILD"
scan() { # scan <xml> <prefix>
    wayland-scanner client-header "$1" "$TOOLS_BUILD/$2-client-protocol.h"
    wayland-scanner private-code "$1" "$TOOLS_BUILD/$2-client-protocol.c"
}
scan "$WP/stable/xdg-shell/xdg-shell.xml" xdg-shell
scan "$WP/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml" xdg-toplevel-icon-v1
scan "$WP/staging/xdg-system-bell/xdg-system-bell-v1.xml" xdg-system-bell-v1
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/Tools/xdg-icon-bell-test.c" \
    "$TOOLS_BUILD/xdg-shell-client-protocol.c" \
    "$TOOLS_BUILD/xdg-toplevel-icon-v1-client-protocol.c" \
    "$TOOLS_BUILD/xdg-system-bell-v1-client-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/xdg-icon-bell-test"

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-iconbell.XXXXXX")"
chmod 700 "$R"
SOCK="sde-iconbell-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$R" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
CP=$!
trap 'kill "$CP" 2>/dev/null || true; rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" 2>/dev/null || true' EXIT
for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
sleep 0.5

echo "=== a client sets an icon and rings the bell ==="
env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/xdg-icon-bell-test" > "$R/tool.log" 2>&1 || true
sleep 0.3

fail=0
check() {
    if grep -qF "$1" "$R/comp.log"; then echo "  ok: $2"; else echo "  FAIL: $2"; fail=1; fi
}
check "xdg-toplevel-icon set" "the client's xdg-toplevel-icon-v1 icon was applied"
check "xdg-system-bell ring" "the client's xdg-system-bell-v1 ring was handled"
if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$R/comp.log"; then echo "  FAIL: the compositor faulted"; fail=1; fi

if [[ "$fail" -ne 0 ]]; then echo "==> icon/bell test FAILED"; exit 1; fi
echo "==> icon/bell test PASSED"
