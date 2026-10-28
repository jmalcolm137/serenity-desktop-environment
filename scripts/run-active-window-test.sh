#!/usr/bin/env bash
# ci-tier: full
# Active-window regression: exactly one window is active at a time.
#
# The compositor used to deactivate the previous window only when the seat still
# had a focused surface. A seat with no keyboard (headless, or the first windows of
# a session) has none, so every window that mapped kept its active frame — and its
# raised Taskbar button — while it was never focused: the demo scene showed several
# windows with active titlebars. Focus now scans the window list
# (deactivate_other_windows) and reports the change to the WindowManagerServer.
# Runs headless (no keyboard: the precondition that used to break it).
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

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

NATIVE="$SDE_ROOT/build/tools/native-app-test"
[[ -x "$NATIVE" ]] || "$SDE_ROOT/scripts/build-tools.sh" >/dev/null 2>&1 || true
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }
command -v xmessage >/dev/null || { echo "SKIP: no xmessage (X11 client)"; exit 0; }

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-active.XXXXXX")"
chmod 700 "$R"
SOCK="sde-active-$$"
env -u WAYLAND_DISPLAY -u DISPLAY -u SDE_XWAYLAND \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$R" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$R/comp.log" 2>&1 &
CP=$!
cleanup() {
    kill "$CP" 2>/dev/null || true
    wait "$CP" 2>/dev/null || true
    rm -rf "$R"
    rm -f "$XDG_RUNTIME_DIR"/sde-active-$$* 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
sleep 0.5
DISP=""
[[ -f "$R/xwayland-display" ]] && DISP="$(cat "$R/xwayland-display")"

fail=0
check() { # check <pattern> <description>
    if grep -aqF "$1" "$R/comp.log"; then echo "  ok: $2"; else echo "  FAIL: $2"; fail=1; fi
}
wait_for_map() { # wait_for_map <title>
    for _ in $(seq 1 60); do grep -aqF "toplevel mapped title='$1'" "$R/comp.log" && return 0; sleep 0.1; done
    return 1
}

# Two xdg (native) clients, then an X11 client, each mapping while the previous is
# still mapped and focused.
env -u WAYLAND_DISPLAY WAYLAND_DISPLAY="$SOCK" "$NATIVE" --title "SDE Active A" --linger 12000 >/dev/null 2>&1 &
A=$!
wait_for_map "SDE Active A" || { echo "  FAIL: window A did not map"; fail=1; }
sleep 0.5

env -u WAYLAND_DISPLAY WAYLAND_DISPLAY="$SOCK" "$NATIVE" --title "SDE Active B" --linger 12000 >/dev/null 2>&1 &
B=$!
wait_for_map "SDE Active B" || { echo "  FAIL: window B did not map"; fail=1; }
for _ in $(seq 1 50); do
    grep -aqF "window 'SDE Active A' deactivated" "$R/comp.log" && break
    sleep 0.1
done
check "window 'SDE Active A' deactivated" "mapping a second window deactivated the first"

if [[ -n "$DISP" ]]; then
    env -u WAYLAND_DISPLAY DISPLAY="$DISP" timeout 15 xmessage -name active-x -geometry 200x90+40+40 "x" >/dev/null 2>&1 &
    X=$!
    for _ in $(seq 1 60); do grep -aqF "X11 window mapped 'active-x'" "$R/comp.log" && break; sleep 0.1; done
    # Focus follows the map asynchronously; wait for the deactivation instead of
    # sampling a fixed delay after the map, which loses under load.
    for _ in $(seq 1 50); do
        grep -aqF "window 'SDE Active B' deactivated" "$R/comp.log" && break
        sleep 0.1
    done
    check "window 'SDE Active B' deactivated" "focusing an X11 window deactivated the xdg window"
    kill "$X" 2>/dev/null || true
else
    echo "  note: no DISPLAY; skipping the X11 leg"
fi

# Exactly one deactivation per focus change (two here) — no window is deactivated
# twice, and the window that ends up focused is never deactivated.
deactivations="$(grep -ac 'deactivated' "$R/comp.log" || true)"
if [[ "$deactivations" == "2" ]]; then
    echo "  ok: exactly one window was deactivated per focus change (2 total)"
else
    echo "  FAIL: expected 2 deactivations, saw $deactivations"; fail=1
fi
if grep -qaE "Assertion|SIGSEGV|Segmentation fault" "$R/comp.log"; then
    echo "  FAIL: the compositor faulted"; fail=1
fi
kill "$A" "$B" 2>/dev/null || true
wait "$A" "$B" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then echo "==> active-window test FAILED"; exit 1; fi
echo "==> active-window test PASSED"
