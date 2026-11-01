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
. "$SDE_ROOT/scripts/lib/test-env.sh"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init selection
sde_test_export_roots

command -v wl-copy >/dev/null && command -v wl-paste >/dev/null || { echo "SKIP: wl-clipboard not installed"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

# SDE_XWAYLAND is cleared, not 0, as before: the compositor's default.
sde_test_compositor SDE_XWAYLAND=
sleep 0.5

echo "=== CLIPBOARD (wl_data_device) ==="
env WAYLAND_DISPLAY="$SOCK" wl-copy "sde-clipboard-test" 2>/dev/null || true
sleep 1
got="$(env WAYLAND_DISPLAY="$SOCK" timeout 3 wl-paste 2>/dev/null | head -1)"
sde_test_expect "wl-paste round-trips the clipboard (got '$got')" test "$got" = "sde-clipboard-test"

echo "=== PRIMARY (zwp_primary_selection_v1) ==="
env WAYLAND_DISPLAY="$SOCK" wl-copy -p "sde-primary-test" 2>/dev/null || true
sleep 1
got="$(env WAYLAND_DISPLAY="$SOCK" timeout 3 wl-paste -p 2>/dev/null | head -1)"
sde_test_expect "wl-paste -p round-trips the primary selection (got '$got')" test "$got" = "sde-primary-test"

sde_test_no_fault
sde_test_finish
