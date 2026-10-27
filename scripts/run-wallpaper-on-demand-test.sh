#!/usr/bin/env bash
# ci-tier: fast
# The wallpaper must be fetched on demand, not pushed to every GUI client.
#
# The compositor used to send a full copy of the wallpaper when a client bound
# serenity_window_manager. Every applet then kept its own ~3 MB bitmap (and a
# 2560x1600 wallpaper would cost 16 MB per process). Only the desktop/Display
# Settings need the image, so the shim now reads the compositor's persisted raw
# state when get_wallpaper() is first called.
#
# This runs a private headless session with a wallpaper, the Taskbar and (when
# built) the desktop. It asserts:
#   * the Taskbar has no wallpaper-sized anonymous mapping;
#   * the compositor did not push "host wallpaper";
#   * FileManager still reads it on demand (the compositor sees set_wallpaper).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
# Taskbar is built in every configuration and is a plain LibGUI client; before
# this fix it received (and copied) the wallpaper just like an applet.
CLIENT="$BUILD/bin/Taskbar"
DESKTOP="$BUILD/bin/FileManager"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

fail=0
check() { if eval "$2"; then echo "  ok: $1"; else echo "  FAIL: $1"; fail=1; fi; }

R="$(mktemp -d "${TMPDIR:-/tmp}/sde-wallpaper-ondemand.XXXXXX")"
chmod 700 "$R"
export XDG_RUNTIME_DIR="$R/run"
mkdir -p "$XDG_RUNTIME_DIR" "$R/session"
chmod 700 "$XDG_RUNTIME_DIR"
SOCK="sde-wallpaper-ondemand-$$"

STARTUP="$CLIENT"
if [[ -x "$DESKTOP" ]]; then
    STARTUP="$STARTUP & $DESKTOP -d"
fi

setsid env -u WAYLAND_DISPLAY -u DISPLAY \
    XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 SDE_OUTPUT_SCALE=1 SDE_XWAYLAND=0 \
    SDE_STATE_DIR="$R" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    SDE_WALLPAPER=blue-grid.png SDE_WALLPAPER_MODE=fill \
    LD_LIBRARY_PATH="$BUILD/lib" \
    SDE_APP_BIN_DIR="$BUILD/bin" SDE_SESSION_BIN_DIR="$BUILD/bin" \
    SDE_SESSION_DIR="$R/session" SDE_WM_PORTAL_PATH="$R/wm.sock" \
    "$COMP" -n "$SOCK" -s "$STARTUP" > "$R/comp.log" 2>&1 &
CP=$!
cleanup() {
    kill -TERM -- "-$CP" 2>/dev/null || true
    sleep 0.2
    kill -KILL -- "-$CP" 2>/dev/null || true
    rm -rf "$R"
    rm -f "$XDG_RUNTIME_DIR/$SOCK" "$XDG_RUNTIME_DIR/$SOCK.lock" 2>/dev/null || true
}
trap cleanup EXIT

for _ in $(seq 1 120); do [[ -S "$XDG_RUNTIME_DIR/$SOCK" ]] && break; sleep 0.1; done
if [[ ! -S "$XDG_RUNTIME_DIR/$SOCK" ]]; then
    echo "==> wallpaper on-demand test FAILED: the compositor did not start"
    tail -20 "$R/comp.log"
    exit 1
fi

# Let the Taskbar bind and map (it loads fonts first on a cold CI image).
CLIENT_PID=""
for _ in $(seq 1 100); do
    for p in $(pgrep -x "Taskbar" 2>/dev/null || true); do
        if tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"; then
            CLIENT_PID="$p"
            break
        fi
    done
    [[ -n "$CLIENT_PID" ]] && break
    sleep 0.1
done

# FileManager reads the wallpaper when its desktop starts; wait for the
# compositor to see the re-apply rather than assuming a fixed startup time.
if [[ -x "$DESKTOP" ]]; then
    for _ in $(seq 1 100); do
        grep -q 'wallpaper set by a client' "$R/comp.log" && break
        sleep 0.1
    done
fi

check "the wallpaper state was persisted" "[[ -s '$R/wallpaper.raw' ]]"
check "no client received a pushed wallpaper" "! grep -q 'LibWM: host wallpaper' '$R/comp.log'"
if [[ -n "$CLIENT_PID" ]]; then
    # A wallpaper copy is an anonymous mapping of (roughly) width*height*4 bytes.
    # The Taskbar is a thin panel; any resident anonymous mapping this large is
    # the regressed eager copy.
    BIG="$(awk '/^[0-9a-f]+-[0-9a-f]+ /{addr=$0} /^Rss:/{if($2 >= 3000) print $2" kB "addr}' "/proc/$CLIENT_PID/smaps" 2>/dev/null | head -1)"
    check "the test client has no wallpaper-sized copy" "[[ -z '$BIG' ]]"
    if [[ -n "$BIG" ]]; then echo "    unexpected mapping: $BIG"; fi
else
    echo "  FAIL: the test client did not map a window"
    fail=1
fi

if [[ -x "$DESKTOP" ]]; then
    check "FileManager read the wallpaper on demand" "grep -q 'wallpaper set by a client' '$R/comp.log'"
else
    echo "  skip: FileManager was not built"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "--- compositor log ---"
    tail -40 "$R/comp.log"
    echo "==> wallpaper on-demand test FAILED"
    exit 1
fi
echo "==> wallpaper on-demand test PASSED"
