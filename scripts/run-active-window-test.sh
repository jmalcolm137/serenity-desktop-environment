#!/usr/bin/env bash
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
env -u WAYLAND_DISPLAY -u DISPLAY \
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
sleep 0.5
check "window 'SDE Active A' deactivated" "mapping a second window deactivated the first"

if [[ -n "$DISP" ]]; then
    env -u WAYLAND_DISPLAY DISPLAY="$DISP" timeout 15 xmessage -name active-x -geometry 200x90+40+40 "x" >/dev/null 2>&1 &
    X=$!
    for _ in $(seq 1 60); do grep -aqF "X11 window mapped 'active-x'" "$R/comp.log" && break; sleep 0.1; done
    sleep 0.5
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
