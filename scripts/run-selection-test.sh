#!/usr/bin/env bash
# ci-tier: full
# Selection test: the compositor forwards CLIPBOARD (wl_data_device) and PRIMARY
# (zwp_primary_selection_v1 / gtk) selection requests to the wlr_seat, which is
# what lets wlroots' XWM bridge X11 PRIMARY/CLIPBOARD with the Wayland selection.
# A Wayland client sets each and reads it back (wl-clipboard, if installed).
set -euo pipefail

# This test starts an X server, and an X server's sockets are *abstract*
# (@/tmp/.X11-unix/X<n>), so they are shared with every other X server on the
# machine and namespaced by the network namespace rather than the filesystem. Two
# Xwaylands fighting over a display name killed a live session's X server, and a
# missing liveness check in the compositor turned that into a crash
# (docs/26-ci.md §6). Re-exec inside a private network namespace so the test's X
# server cannot be seen by — or collide with — a session's.
if [[ "${SDE_TEST_IN_NAMESPACE:-0}" != 1 ]]; then
    if unshare -Urnm true 2>/dev/null; then
        exec unshare -Urnm env SDE_TEST_IN_NAMESPACE=1 "$0" "$@"
    fi
    # No unprivileged namespaces (some containers): only safe with no session.
    if [[ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/sde" ]]; then
        echo "SKIP: a live session owns the X display and namespaces are unavailable"
        exit 0
    fi
else
    # A private, correctly-owned X socket directory: Xwayland refuses a
    # /tmp/.X11-unix it does not own, and the host's is owned by root, which is an
    # unmapped (nobody) uid inside a user namespace.
    mkdir -p /tmp/.X11-unix
    mount -t tmpfs -o mode=1777 none /tmp/.X11-unix 2>/dev/null || true
fi

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

command -v wl-copy >/dev/null && command -v wl-paste >/dev/null || { echo "SKIP: wl-clipboard not installed"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-selection.XXXXXX")"
chmod 700 "$R"
SOCK="sde-selection-$$"
env -u WAYLAND_DISPLAY -u DISPLAY -u SDE_XWAYLAND \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$R" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
CP=$!
trap 'kill "$CP" 2>/dev/null || true; rm -rf "$R"; rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" 2>/dev/null || true' EXIT
for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
sleep 0.5

fail=0
check() {
    if [[ "$2" == "$3" ]]; then echo "  ok: $1 ($3)"; else echo "  FAIL: $1 (got '$2', want '$3')"; fail=1; fi
}

echo "=== CLIPBOARD (wl_data_device) ==="
env WAYLAND_DISPLAY="$SOCK" wl-copy "sde-clipboard-test" 2>/dev/null || true
sleep 1
check "wl-paste round-trips the clipboard" "$(env WAYLAND_DISPLAY="$SOCK" timeout 3 wl-paste 2>/dev/null | head -1)" "sde-clipboard-test"

echo "=== PRIMARY (zwp_primary_selection_v1) ==="
env WAYLAND_DISPLAY="$SOCK" wl-copy -p "sde-primary-test" 2>/dev/null || true
sleep 1
check "wl-paste -p round-trips the primary selection" "$(env WAYLAND_DISPLAY="$SOCK" timeout 3 wl-paste -p 2>/dev/null | head -1)" "sde-primary-test"

if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$R/comp.log"; then echo "  FAIL: the compositor faulted"; fail=1; fi

if [[ "$fail" -ne 0 ]]; then echo "==> selection test FAILED"; exit 1; fi
echo "==> selection test PASSED"
