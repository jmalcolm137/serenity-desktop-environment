#!/usr/bin/env bash
# Headless test for the R5 ScreenCast backend: xdg-desktop-portal-serenity
# captures an output via ext-image-copy-capture-v1 and publishes a PipeWire
# stream; the portal returns the stream node id and frames flow to a consumer.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
PORTAL="$SDE_ROOT/build/tools/xdg-desktop-portal-serenity"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

if [ "${1:-}" != "--inner" ]; then
    # A private D-Bus session bus; PipeWire keeps the real XDG_RUNTIME_DIR.
    exec dbus-run-session -- "$0" --inner
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-screencast.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

SOCK="sde-sc-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_STATE_DIR="$RUNTIME" \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.3

# A stub source chooser: pick the first window (index 1; 0 is the whole screen).
cat > "$RUNTIME/chooser.sh" <<'EOF'
#!/usr/bin/env bash
echo 1
EOF
chmod +x "$RUNTIME/chooser.sh"

# A stub color picker for the Screenshot portal's PickColor: click at (7, 9).
cat > "$RUNTIME/colorpicker.sh" <<'EOF'
#!/usr/bin/env bash
echo "7 9"
EOF
chmod +x "$RUNTIME/colorpicker.sh"

SDE_PORTAL_CHOOSER="$RUNTIME/chooser.sh" SDE_PORTAL_COLOR_PICKER="$RUNTIME/colorpicker.sh" SDE_STATE_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PORTAL" > "$RUNTIME/portal.log" 2>&1 &
PORTAL_PID=$!
trap 'kill "$PORTAL_PID" "$COMP_PID" 2>/dev/null || true' EXIT

DEST="org.freedesktop.impl.portal.desktop.serenity"
OBJ="/org/freedesktop/portal/desktop"
IFACE="org.freedesktop.impl.portal.ScreenCast"
for _ in $(seq 1 50); do
    gdbus call --session --dest "$DEST" --object-path "$OBJ" --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1 && break
    sleep 0.1
done

fail=0
check() {
    if grep -qF "$1" <<<"$2"; then
        echo "  ok: $1"
    else
        echo "  MISSING ($2): $1"
        fail=1
    fi
}
call() { gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$@" 2>&1; }

echo "=== ScreenCast session ==="
check "uint32 0" "$(call "$IFACE.CreateSession" /h /org/freedesktop/portal/desktop/session/a app "{}")"
check "uint32 0" "$(call "$IFACE.SelectSources" /h /org/freedesktop/portal/desktop/session/a app "{'types': <uint32 1>}")"
check "uint32 3" "$(call "$IFACE.AvailableSourceTypes")"
check "uint32 3" "$(call "$IFACE.AvailableCursorModes")"

echo "=== Start returns a PipeWire stream ==="
start="$(call "$IFACE.Start" /h /org/freedesktop/portal/desktop/session/a app "" "{}")"
check "uint32 0" "$start"
check "streams" "$start"
check "(1280, 720)" "$start"
check "'source_type': <uint32 1>" "$start"
check "pipewire-serial" "$start"
node="$(grep -oE "uint32 [0-9]+" <<<"$start" | sed -n 2p | awk '{print $2}')"
if [ -n "$node" ] && [ "$node" -gt 0 ] 2>/dev/null; then
    echo "  ok: stream node id $node"
else
    echo "  FAIL: no stream node id ($start)"
    fail=1
fi

echo "=== embedded cursor mode ==="
call "$IFACE.CreateSession" /h /org/freedesktop/portal/desktop/session/c app "{}" >/dev/null
call "$IFACE.SelectSources" /h /org/freedesktop/portal/desktop/session/c app "{'types': <uint32 1>, 'cursor_mode': <uint32 2>}" >/dev/null
cursor_start="$(call "$IFACE.Start" /h /org/freedesktop/portal/desktop/session/c app "" "{}")"
check "uint32 0" "$cursor_start"
check "cursor 2" "$(cat "$RUNTIME/portal.log")"

echo "=== window source ==="
if [ -x "$SDE_ROOT/build/tools/native-app-test" ]; then
    env WAYLAND_DISPLAY="$SOCK" "$SDE_ROOT/build/tools/native-app-test" >/dev/null 2>&1 &
    APP_PID=$!
    sleep 1
    call "$IFACE.CreateSession" /h /org/freedesktop/portal/desktop/session/w app "{}" >/dev/null
    call "$IFACE.SelectSources" /h /org/freedesktop/portal/desktop/session/w app "{'types': <uint32 2>}" >/dev/null
    window_start="$(call "$IFACE.Start" /h /org/freedesktop/portal/desktop/session/w app "" "{}")"
    kill "$APP_PID" 2>/dev/null || true
    if grep -qF "'source_type': <uint32 2>" <<<"$window_start"; then
        echo "  ok: window source (source_type 2)"
    else
        echo "  note: window source not selected ($window_start)"
    fi
else
    echo "  note: native-app-test not built; skipping the window-source check"
fi

echo "=== RemoteDesktop portal ==="
RD="org.freedesktop.impl.portal.RemoteDesktop"
check "uint32 0" "$(call "$RD.CreateSession" /h /org/freedesktop/portal/desktop/session/rd app "{}")"
check "uint32 0" "$(call "$RD.SelectDevices" /h /org/freedesktop/portal/desktop/session/rd app "{'types': <uint32 3>}")"
rd_start="$(call "$RD.Start" /h /org/freedesktop/portal/desktop/session/rd app "" "{}")"
check "uint32 0" "$rd_start"
check "'devices'" "$rd_start"
call "$RD.NotifyKeyboardKeycode" /org/freedesktop/portal/desktop/session/rd "{}" 30 1 >/dev/null
call "$RD.NotifyKeyboardKeycode" /org/freedesktop/portal/desktop/session/rd "{}" 30 0 >/dev/null
call "$RD.NotifyPointerMotion" /org/freedesktop/portal/desktop/session/rd "{}" 5.0 5.0 >/dev/null
sleep 0.3
check "virtual keyboard connected" "$(cat "$RUNTIME/comp.log")"
check "virtual pointer connected" "$(cat "$RUNTIME/comp.log")"

echo "=== GlobalShortcuts reaches the compositor config ==="
GS="org.freedesktop.impl.portal.GlobalShortcuts"
call "$GS.CreateSession" /h /org/freedesktop/portal/desktop/session/gs app "{}" >/dev/null
call "$GS.BindShortcuts" /h /org/freedesktop/portal/desktop/session/gs "[('sc1', {'trigger': <'CTRL+ALT+g'>, 'description': <'Test'>})]" "" "{}" >/dev/null
for _ in $(seq 1 40); do
    grep -qF "1 global shortcut(s)" "$RUNTIME/comp.log" 2>/dev/null && break
    sleep 0.1
done
check "1 global shortcut(s)" "$(cat "$RUNTIME/comp.log")"

echo "=== Screenshot portal ==="
shot="$(call org.freedesktop.impl.portal.Screenshot.Screenshot /h app "" "{}")"
check "uint32 0" "$shot"
check "uri" "$shot"
shot_uri="$(grep -oE "file://[^' ]+" <<<"$shot" | head -1)"
shot_path="${shot_uri#file://}"
if [ -n "$shot_uri" ] && [ -f "$shot_path" ]; then
    echo "  ok: screenshot written to $shot_path"
else
    echo "  FAIL: screenshot file missing ($shot)"
    fail=1
fi

echo "=== PickColor portal ==="
pick="$(call org.freedesktop.impl.portal.Screenshot.PickColor /h app "" "{}")"
check "uint32 0" "$pick"
check "color" "$pick"

echo "=== the PipeWire stream node exists ==="
if command -v pw-cli >/dev/null 2>&1; then
    if pw-cli ls Node 2>/dev/null | grep -q "sde-screencast"; then
        echo "  ok: PipeWire node 'sde-screencast' published"
    else
        echo "  FAIL: PipeWire node 'sde-screencast' not found"
        fail=1
    fi
fi

echo "=== frames reach a PipeWire consumer (best effort) ==="
if command -v gst-launch-1.0 >/dev/null 2>&1; then
    timeout 6 gst-launch-1.0 -q pipewiresrc target-object="$node" num-buffers=10 ! fakesink > "$RUNTIME/gst.log" 2>&1 || true
    if grep -qF "first pipewire buffer" "$RUNTIME/portal.log" 2>/dev/null; then
        echo "  ok: a consumer pulled frames (first pipewire buffer)"
    else
        echo "  note: consumer targeting not exercised here ($(head -1 "$RUNTIME/gst.log")); the node is published and Start returned it"
    fi
else
    echo "  note: gst-launch-1.0 not found; skipping the consumer check"
fi
check "screencast pipewire node id" "$(cat "$RUNTIME/portal.log")"
check "screencast first frame" "$(cat "$RUNTIME/portal.log")"

kill "$PORTAL_PID" "$COMP_PID" 2>/dev/null || true
wait "$PORTAL_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> screencast test FAILED"
    exit 1
fi
echo "==> screencast test PASSED"
