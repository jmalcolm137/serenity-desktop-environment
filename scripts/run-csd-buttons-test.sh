#!/usr/bin/env bash
# Client-side decoration buttons (CSD) test.
#
# A client that draws its own titlebar — Firefox, any GTK app with a header bar —
# wires its minimise/maximise buttons to xdg_toplevel requests: set_maximized and
# set_minimized. The compositor has to honour them.
#
# It did not: `toplevel_request_maximize` scheduled a configure without applying
# anything (the request was noted and thrown away, so the button did nothing), and
# there was no handler for set_minimized at all. Reported from a live session as
# "the Firefox buttons in the header do not work: I cannot minimize, maximize or
# close".
#
# This drives both requests from a client and asserts the two halves of the
# handshake: the compositor applied it, and the client was told (the configure it
# receives carries the maximized state, and the size it asked for).
#
# Runs headless and hermetic — its own state dir, session dir and heartbeat, no
# Xwayland — so it is safe on a machine with a live session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-csd.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-csd-$$"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"{,.lock}
}
trap cleanup EXIT

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME/state" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SDE_XWAYLAND=0 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 200); do
    [[ -e "$XDG_RUNTIME_DIR/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.5

fail=0
check() {
    if [[ "$2" == "$3" ]]; then echo "  ok: $1"; else echo "  MISSING: $1 (expected '$2', got '$3')"; fail=1; fi
}

echo "=== a client asks to be maximized (its own titlebar button) ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 8 "$NATIVE" --title "CSD Max" --maximize --linger 4000 > "$RUNTIME/max.log" 2>&1 || true
sleep 0.3
check "the compositor saw the request" "1" "$(grep -c "asked to be maximized" "$RUNTIME/comp.log")"
# The handshake: the client is configured maximized, at the workarea size.
check "the client was configured maximized" "1" "$(grep -c "configured maximized=1" "$RUNTIME/max.log")"
check "at the workarea size" "1" "$(grep -c "configured maximized=1 1280x720" "$RUNTIME/max.log")"
check "the maximized window is at the workarea origin" "1" "$(grep -c "maximized 'CSD Max' at 0,0 size 1280x720" "$RUNTIME/comp.log")"

echo "=== maximize then restore returns to the saved position ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 8 "$NATIVE" --title "CSD Restore" --maximize-restore --linger 3000 > "$RUNTIME/restore.log" 2>&1 || true
sleep 0.3
max_line="$(grep "maximized 'CSD Restore'" "$RUNTIME/comp.log" | tail -1)"
restore_xy="$(sed -nE 's/.*\(restore ([0-9-]+),([0-9-]+) .*/\1,\2/p' <<<"$max_line")"
check "the restore request was seen" "1" "$(grep -c "asked to be restored" "$RUNTIME/comp.log")"
if [[ -n "$restore_xy" && "$restore_xy" != "0,0" ]]; then
    echo "  ok: the maximize saved a non-origin restore position ($restore_xy)"
else
    echo "  MISSING: the maximize did not save a restore position"
    fail=1
fi
check "the window returned to the saved position" "1" "$(grep -c "restored 'CSD Restore' at $restore_xy" "$RUNTIME/comp.log")"
check "the client was configured unmaximized" "1" "$(grep -c "configured maximized=0 320x200" "$RUNTIME/restore.log")"

echo "=== and asks to be minimized ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    timeout 8 "$NATIVE" --title "CSD Min" --minimize --linger 4000 > "$RUNTIME/min.log" 2>&1 || true
sleep 0.3
check "the compositor saw the request" "1" "$(grep -c "asked to be minimized" "$RUNTIME/comp.log")"
if kill -0 "$COMP_PID" 2>/dev/null; then echo "  ok: the compositor survived"; else echo "  MISSING: the compositor died"; fail=1; fi

if [[ "$fail" -eq 0 ]]; then
    echo "==> CSD buttons test PASSED"
else
    echo "==> CSD buttons test FAILED"
    echo "--- client log ---"; tail -5 "$RUNTIME/max.log" | sed 's/^/  /'
    echo "--- compositor log ---"; tail -20 "$RUNTIME/comp.log" | sed 's/^/  /'
    exit 1
fi
