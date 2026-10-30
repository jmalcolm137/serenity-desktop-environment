#!/usr/bin/env bash
# ci-tier: full
# XWayland regression (R5): an X11 client exiting must NOT abort the compositor.
#
# wlroots' xwayland_surface_destroy() asserts that every listener list on the
# xsurface's events (destroy, set_title, set_hints, ...) is empty once it emits
# "destroy". The compositor therefore has to detach each listener it added; when
# it leaked them, typing "exit" in xterm (or any X11 client going away) aborted
# the whole compositor with:
#   xwayland/xwm.c:581: xwayland_surface_destroy: Assertion
#   `wl_list_empty(&xsurface->events.destroy.listener_list)' failed.
# Runs headless.
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
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

command -v xmessage >/dev/null || { echo "SKIP: no xmessage (X11 client)"; exit 0; }

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-xwayland-exit.XXXXXX")"
chmod 700 "$R"
SOCK="sde-xwayland-exit-$$"

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
    rm -f "$XDG_RUNTIME_DIR"/sde-xwayland-exit-$$* 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 80); do [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
sleep 0.5
DISP=""
[[ -f "$R/xwayland-display" ]] && DISP="$(cat "$R/xwayland-display")"
[[ -n "$DISP" ]] || { echo "==> xwayland-exit test FAILED (no DISPLAY)"; exit 1; }

fail=0
alive() { kill -0 "$CP" 2>/dev/null; }

# A client mapping and then exiting on its own (the "exit in xterm" path).
env -u WAYLAND_DISPLAY DISPLAY="$DISP" \
    timeout 20 xmessage -name exit-test -geometry 120x60+40+40 "bye" >/dev/null 2>&1 &
C1=$!
for _ in $(seq 1 40); do grep -q "X11 window mapped 'exit-test'" "$R/comp.log" && break; sleep 0.1; done
if grep -q "X11 window mapped 'exit-test'" "$R/comp.log"; then
    echo "  ok: the X11 client mapped a window"
else
    echo "  FAIL: the X11 client never mapped a window"; fail=1
fi
# A client with no icon of its own is still reported to the WM with the default
# icon (the Taskbar drops an icon that arrives before the window button exists, so
# this must reach it after the window state).
if grep -qE "WM: X11 window [0-9]+ icon 16x16" "$R/comp.log"; then
    echo "  ok: an iconless X11 window reports the default icon to the WM"
else
    echo "  FAIL: the default X11 icon was not reported to the WM"; fail=1
fi
kill -TERM "$C1" 2>/dev/null || true
wait "$C1" 2>/dev/null || true
sleep 1
if alive; then
    echo "  ok: the compositor survived the client exiting"
else
    echo "  FAIL: the compositor died when the client exited"; fail=1
fi

# Repeated map/exit cycles: the listener bookkeeping must not leak or double-add.
cycles_ok=1
for i in 1 2 3 4 5; do
    env -u WAYLAND_DISPLAY DISPLAY="$DISP" \
        timeout 20 xmessage -name "cycle-$i" -geometry 100x40+40+$((40 * i)) "x" >/dev/null 2>&1 &
    P=$!
    sleep 0.4
    kill -TERM "$P" 2>/dev/null || true
    wait "$P" 2>/dev/null || true
    if ! alive; then echo "  FAIL: the compositor died on exit cycle $i"; fail=1; cycles_ok=0; break; fi
done
[[ "$cycles_ok" -eq 1 ]] && echo "  ok: the compositor survived 5 map/exit cycles"

# The X server itself going away with a window mapped: wlroots tears down its
# window manager and dissociates every X11 window, and the compositor must carry
# on without them. (The opposite order, Xwayland's Wayland connection closing
# while its window manager is still up, is the shutdown path; see
# run-shutdown-test.sh.)
env -u WAYLAND_DISPLAY DISPLAY="$DISP" \
    timeout 20 xmessage -name xserver-gone -geometry 120x60+40+40 "x" >/dev/null 2>&1 &
C2=$!
for _ in $(seq 1 40); do grep -q "X11 window mapped 'xserver-gone'" "$R/comp.log" && break; sleep 0.1; done
# Find the server, not the instant it was started: under load Xwayland can
# restart between the map and this lookup, and a single pgrep then sees no
# process (the window has mapped, but the new server is not in /proc yet).
XPID=""
for _ in $(seq 1 50); do
    XPID="$(pgrep -f "Xwayland $DISP " | head -1 || true)"
    [[ -n "$XPID" ]] && break
    sleep 0.1
done
if [[ -n "$XPID" ]] && grep -q "X11 window mapped 'xserver-gone'" "$R/comp.log"; then
    kill -KILL "$XPID" 2>/dev/null || true
    sleep 1
    if alive; then
        echo "  ok: the compositor survived the X server going away with a window mapped"
    else
        echo "  FAIL: the compositor died when the X server went away"; fail=1
    fi
else
    echo "  FAIL: no X server process or mapped window to take away (pid '${XPID}')"; fail=1
fi
wait "$C2" 2>/dev/null || true

# The wlroots assertion must not have fired, and the compositor must not have faulted.
# (Note: benign "xcb error ... code Window" lines at xwm.c:1984 appear when a client
# exits with requests in flight — only the listener assertion is fatal.)
if grep -qiE "Assertion|wl_list_empty|listener_list" "$R/comp.log"; then
    echo "  FAIL: a wlroots listener assertion fired:"; grep -aiE "Assertion|wl_list_empty|listener_list" "$R/comp.log" | head -2; fail=1
else
    echo "  ok: no wlroots listener assertion"
fi
if grep -qiE "SIGSEGV|Segmentation fault|ASSERTION FAILED|VERIFICATION FAILED" "$R/comp.log"; then
    echo "  FAIL: the compositor faulted"; fail=1
fi

if [[ "$fail" -ne 0 ]]; then echo "==> xwayland-exit test FAILED"; exit 1; fi
echo "==> xwayland-exit test PASSED"
