#!/usr/bin/env bash
# Multi-output / hotplug test (R6/N5).
#
# A disabled output still exists, but nothing of it is on screen any more — the
# same state a client sees when a monitor is unplugged. Two things must then be
# true, and neither was:
#
#   * the windows that were on it must move to a surviving output. Window
#     positions are layout coordinates, so a window left behind sits outside every
#     output: invisible, and unreachable without editing the layout by hand;
#   * the primary output must be reassigned. It is the reference for the workarea
#     and for placing new windows, and every placement after losing it would use a
#     destroyed output.
#
# Two headless outputs, a window on the primary, then disable the primary through
# `zwlr_output_management_v1` (what Display Settings and `wlr-randr` do) and
# re-enable it. Runs entirely headless, and hermetic: its own state dir, session
# dir and heartbeat, and no Xwayland, so it is safe to run on a machine with a
# live session.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
OM="$SDE_ROOT/build/tools/output-management-test"
NATIVE="$SDE_ROOT/build/tools/native-app-test"
XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export XDG_RUNTIME_DIR

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null
[[ -x "$OM" ]] || { echo "SKIP: output-management-test not built"; exit 0; }
[[ -x "$NATIVE" ]] || { echo "SKIP: native-app-test not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-hotplug.XXXXXX")"
chmod 700 "$RUNTIME"
SOCK="sde-hotplug-$$"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill -TERM -- "-$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && { sleep 0.3; kill -KILL -- "-$COMP_PID" 2>/dev/null || true; }
    rm -rf "$RUNTIME"
    rm -f "$XDG_RUNTIME_DIR/$SOCK"{,.lock}
}
trap cleanup EXIT

# A known layout, written rather than discovered: HEADLESS-2 is the configured main
# screen (so it is the primary, and the test can watch the setting be honoured), and
# HEADLESS-1 is deliberately a different size, so a window that moves onto it has to
# be re-fitted rather than just repositioned.
mkdir -p "$RUNTIME/state"
cat > "$RUNTIME/state/screenlayout.ini" <<'INI'
[Screens]
MainScreen=0

[Screen0]
Device=HEADLESS-2
Width=1280
Height=720
Top=0
Left=0
Mode=Device
ScaleFactor=1

[Screen1]
Device=HEADLESS-1
Width=800
Height=600
Top=1280
Left=0
Mode=Device
ScaleFactor=1
INI

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 SDE_OUTPUT_SCALE=1 \
    SDE_STATE_DIR="$RUNTIME/state" \
    SDE_SESSION_DIR="$RUNTIME/session" \
    SDE_HEARTBEAT_FILE="$RUNTIME/heartbeat" \
    SDE_XWAYLAND=0 \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

# Wait for the *layout* to have been applied, not just for the outputs to exist:
# the primary is the one the layout puts at 0,0, and those lines come later.
for _ in $(seq 1 200); do
    [[ "$(grep -c 'applied screen layout' "$RUNTIME/comp.log" 2>/dev/null)" -ge 2 ]] && break
    sleep 0.1
done
OUTPUTS="$(grep -o 'HEADLESS-[0-9]*' "$RUNTIME/comp.log" 2>/dev/null | sort -u | tr '\n' ' ')"
# The primary is the first output created, and the layout places it at 0,0. Do not
# assume which name that is: wlroots creates HEADLESS-2 before HEADLESS-1.
PRIMARY="$(sed -n "s/.*applied screen layout to '\(HEADLESS-[0-9]*\)': .* at 0,0.*/\1/p" "$RUNTIME/comp.log" | head -1)"
SURVIVOR="$(grep -o 'HEADLESS-[0-9]*' "$RUNTIME/comp.log" | sort -u | grep -v "^${PRIMARY}$" | head -1)"
echo "=== outputs: ${OUTPUTS:-none}  (primary ${PRIMARY:-unknown}, survivor ${SURVIVOR:-unknown}) ==="
if [[ -z "$PRIMARY" || -z "$SURVIVOR" ]]; then
    echo "  MISSING: could not tell the primary from the log"; tail -20 "$RUNTIME/comp.log"; exit 1
fi

fail=0
check() { # description, expected, actual
    if [[ "$2" == "$3" ]]; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1 (expected '$2', got '$3')"
        fail=1
    fi
}
check "two outputs came up" "2" "$(grep -o 'HEADLESS-[0-9]*' "$RUNTIME/comp.log" | sort -u | wc -l | tr -d ' ')"

echo "=== a window on the primary ==="
env WAYLAND_DISPLAY="$SOCK" LD_LIBRARY_PATH="$BUILD/lib" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    "$NATIVE" --title "Hotplug A" --maximize --linger 20000 > "$RUNTIME/app.log" 2>&1 &
APP_PID=$!
for _ in $(seq 1 100); do
    grep -q "toplevel mapped" "$RUNTIME/comp.log" && break
    sleep 0.1
done
check "the window mapped" "1" "$(grep -c 'toplevel mapped' "$RUNTIME/comp.log")"

echo "=== disable the primary ($PRIMARY), which is where the window is ==="
if env WAYLAND_DISPLAY="$SOCK" "$OM" --off "$PRIMARY" > "$RUNTIME/om.log" 2>&1; then
    echo "  ok: the compositor accepted the configuration"
else
    echo "  MISSING: the compositor refused it:"; sed 's/^/    /' "$RUNTIME/om.log" | head -5; fail=1
fi
sleep 0.5
check "the output is disabled" "1" "$(grep -c "output '$PRIMARY' disabled" "$RUNTIME/comp.log")"
check "its window moved to the survivor" "1" \
    "$(grep -c "moved [0-9]* window(s) off the removed output onto '$SURVIVOR'" "$RUNTIME/comp.log")"
check "the primary was reassigned" "1" "$(grep -c "primary output is now '$SURVIVOR'" "$RUNTIME/comp.log")"
# It was maximised on 1280x720; the survivor is 800x600, so it has to be re-fitted,
# not merely moved.
check "the maximised window was re-fitted" "1" "$(grep -c "configured maximized=1 800x600" "$RUNTIME/app.log")"
if kill -0 "$COMP_PID" 2>/dev/null; then echo "  ok: the compositor survived"; else echo "  MISSING: the compositor died"; fail=1; fi

echo "=== re-enable it ==="
# A disabled head advertises no modes, so the size comes from the layout line.
PRIMARY_MODE="$(sed -n "s/.*applied screen layout to '$PRIMARY': \([0-9]*x[0-9]*\).*/\1/p" "$RUNTIME/comp.log" | head -1)"
# Note: re-enabling does not make it primary again — the primary stays wherever
# it was reassigned. Serenity's MainScreen index would say otherwise; that is a
# follow-up, and this test asserts what is true today.
if env WAYLAND_DISPLAY="$SOCK" "$OM" --on "$PRIMARY" "${PRIMARY_MODE:-1280x720}" > "$RUNTIME/om2.log" 2>&1; then
    echo "  ok: the compositor accepted the configuration"
else
    echo "  MISSING: the compositor refused it:"; sed 's/^/    /' "$RUNTIME/om2.log" | head -5; fail=1
fi
sleep 0.5
if kill -0 "$COMP_PID" 2>/dev/null; then echo "  ok: the compositor survived the re-enable"; else echo "  MISSING: the compositor died"; fail=1; fi
# And the configured main screen takes the primacy back (Serenity's MainScreen).
check "the configured main screen is primary again" "1" \
    "$(grep -c "primary output is now '$PRIMARY' (the configured main screen)" "$RUNTIME/comp.log")"

kill "$APP_PID" 2>/dev/null || true
if [[ "$fail" -eq 0 ]]; then
    echo "==> output hotplug test PASSED"
else
    echo "==> output hotplug test FAILED"
    echo "--- compositor log (tail) ---"; tail -25 "$RUNTIME/comp.log" | sed 's/^/  /'
    exit 1
fi
