#!/usr/bin/env bash
# Headless notification test (docs/05-r3-plan.md §4 WS5): run LibGUI::Notification
# through the notify portal, which spawns the unmodified SerenityOS
# NotificationServer, and assert that the compositor creates the notification
# banners as undecorated overlay layer surfaces ("serenity-notification"), above
# normal windows and out of the window list/switcher.
#
# Uses a private session directory, so it never touches (or gets answered by) a
# live SDE session. Runs entirely headless (no host compositor, no DRM).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TEST="$SDE_ROOT/build/test/libwm-notify-test"

echo "== building prerequisites =="
"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-services.sh" >/dev/null 2>&1 || true
"$SDE_ROOT/scripts/build-tests.sh" libwm-notify-test >/dev/null 2>&1 || true

# NotificationServer is an SDE service (build-services.sh puts it in the SDE build
# tree, not the framework's bin/). Assemble the session app directory the way a
# session does, so LibWM finds it by name wherever it was built.
APP_BIN="$(mktemp -d "${TMPDIR:-/tmp}/sde-notify-bin.XXXXXX")"
for service in NotificationServer; do
    [[ -x "$SDE_ROOT/build/$service/$service" ]] && ln -sf "$SDE_ROOT/build/$service/$service" "$APP_BIN/$service"
done
[[ -x "$SDE_ROOT/build/NotificationServer/NotificationServer" ]] || { echo "SKIP: NotificationServer not built"; exit 0; }

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-notify.XXXXXX")"
export SDE_STATE_DIR="$RUNTIME"
chmod 700 "$RUNTIME"
trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME" "$APP_BIN"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

# A private per-session runtime directory (the SDE_SESSION_DIR seam), so the
# notify portal sockets of a live session are left alone.
SESSION_DIR="$RUNTIME/session"
mkdir -p "$SESSION_DIR/portal"
export SDE_SESSION_DIR="$SESSION_DIR"

SOCK="sde-notify-$$"

env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless \
    WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!

for _ in $(seq 1 50); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 1

# A notification with an icon + launch URL, an iconless one, and an in-place
# update - the whole LibGUI::Notification surface, through the notify portal.
env WAYLAND_DISPLAY="$SOCK" \
    SDE_SESSION_DIR="$SESSION_DIR" \
    SDE_APP_BIN_DIR="$APP_BIN" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    timeout 15 "$TEST" > "$RUNTIME/client.log" 2>&1 || true

sleep 1
kill "$COMP_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

echo "--- client log ---"
cat "$RUNTIME/client.log"
echo "--- compositor log ---"
cat "$RUNTIME/comp.log"

fail=0
check() {
    if grep -qF "$1" "$RUNTIME/client.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING (client): $1"
        fail=1
    fi
    return 0
}
check_comp() {
    if grep -qF "$1" "$RUNTIME/comp.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING (compositor): $1"
        fail=1
    fi
    return 0
}

echo "=== notification test ==="
# The notify portal deferred to the spawned unmodified NotificationServer...
check "deferring to the session service on '$SESSION_DIR/portal/notify'"
check "spawned notification pid="
# ...and LibGUI::Notification round-tripped (all three lifecycle calls succeed).
check "NOTIFY-TEST: shown 1 icon=yes launch=yes is_showing=true"
check "NOTIFY-TEST: shown 2 icon=no is_showing=true"
check "NOTIFY-TEST: updated 1 is_showing=true"

# The compositor turned each notification into an overlay banner layer surface.
# Overlay => above normal windows; layer surface => not a toplevel, so it is not
# in the window list or the switcher.
check_comp "layer surface namespace='serenity-notification' layer=overlay"

banner_count="$(grep -cF "layer surface namespace='serenity-notification'" "$RUNTIME/comp.log" || true)"
if [[ "$banner_count" -ge 2 ]]; then
    echo "  ok: two banners (count=$banner_count)"
else
    echo "  MISSING: two banners (count=$banner_count)"
    fail=1
fi

# A notification must never be a managed toplevel (that would put it in the
# window list and the switcher).
if grep -qF "toplevel mapped" "$RUNTIME/comp.log"; then
    echo "  UNEXPECTED: notification created a managed toplevel"
    fail=1
else
    echo "  ok (absent): no managed toplevel for the notifications"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> notification test FAILED"
    exit 1
fi
echo "==> notification test PASSED"
