#!/usr/bin/env bash
# A client whose compositor goes away must exit cleanly.
#
# When the compositor dies, the shim's WindowManagerServer connection dies, and
# LibGUI responds by calling exit() — that is the designed behaviour. What was not
# designed is what happened during that exit: `GUI::Application` had already
# destroyed its EventLoop while widget trees were still being destroyed, and
# destroying a widget can defer work (a relayout). `EventLoop::current()` then found
# an empty loop stack and returned a reference into it:
#
#   EventLoop::current()            ← SIGSEGV
#     ← deferred_invoke ← Window::schedule_relayout
#       ← Widget::child_event ← remove_child ← remove_from_parent
#         ← ~NonnullOwnPtr<Window> ← exit ← ConnectionToServer::die
#           ← EventLoop::exec
#
# So every compositor restart dumped core in every client whose teardown defers,
# which is why a restart looked like a crash storm. The fix is
# patches/0055-eventloop-tolerate-teardown.patch (an inert loop instead of a
# reference into the empty stack): work deferred during teardown is never run, which
# is what teardown wants.
#
# This starts a compositor, a LibGUI client (the Taskbar), kills the compositor the
# way the session supervisor does, and checks the *client's* exit status: dying by
# SIGSEGV (139) is the bug, exiting by any ordinary means is correct. Runs headless
# and hermetic (its own runtime dir, so it cannot touch a live session's sockets).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
CLIENT="$BUILD/bin/Taskbar"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
[[ -x "$CLIENT" ]] || { echo "SKIP: the Taskbar is not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-disconnect.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-disconnect-$$"
COMP_PID=""
CLIENT_PID=""
cleanup() {
    [[ -n "$CLIENT_PID" ]] && kill -TERM "$CLIENT_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

# Everything isolated: the client must not reach a live session's portals either,
# which is why it gets SDE_SESSION_DIR as well as the compositor.
setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    XDG_RUNTIME_DIR="$RUNTIME" \
    SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" SDE_XWAYLAND=0 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 200); do
    [[ -e "$RUNTIME/$SOCK" ]] && break
    sleep 0.1
done

env WAYLAND_DISPLAY="$SOCK" XDG_RUNTIME_DIR="$RUNTIME" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_STATE_DIR="$RUNTIME/state" SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" SDE_XWAYLAND=0 \
    "$CLIENT" > "$RUNTIME/client.log" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 1 200); do
    grep -q "serenity-taskbar" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
if ! grep -q "serenity-taskbar" "$RUNTIME/comp.log" 2>/dev/null; then
    echo "  MISSING: the client never mapped (see $RUNTIME/client.log)"
    tail -5 "$RUNTIME/client.log" | sed 's/^/    /'
    exit 1
fi
echo "  ok: the client is up"

echo "=== the compositor goes away (what the session supervisor does) ==="
kill -TERM -- "-$COMP_PID" 2>/dev/null || true
status=""
for _ in $(seq 1 100); do
    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
        wait "$CLIENT_PID" 2>/dev/null && status=0 || status=$?
        break
    fi
    sleep 0.1
done

if [[ -z "$status" ]]; then
    echo "  MISSING: the client is still running 10s after the compositor left"
    exit 1
fi
if (( status >= 128 )); then
    echo "  MISSING: the client died from signal $((status - 128)) (SIGSEGV is 11, i.e. status 139)"
    echo "    --- the client's own output ---"
    tail -20 "$RUNTIME/client.log" | sed 's/^/    /'
    exit 1
fi
echo "  ok: the client exited cleanly (status $status)"
echo "==> client disconnect test PASSED"
