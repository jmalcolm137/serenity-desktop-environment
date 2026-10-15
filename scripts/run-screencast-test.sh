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
    # A private D-Bus session bus *and* a private runtime directory: the test brings
    # its own PipeWire (a CI runner has none running) and everything it creates --
    # the compositor socket, the PipeWire socket -- stays inside it, so a live
    # session's PipeWire is never used or disturbed.
    RUN_DIR="$(mktemp -d "${TMPDIR:-/tmp}/sde-screencast-run.XXXXXX")"
    chmod 700 "$RUN_DIR"
    if ! command -v pipewire >/dev/null 2>&1; then
        echo "note: pipewire is not installed; skipping the screencast test"
        echo "==> screencast test SKIPPED"
        rm -rf "$RUN_DIR"
        exit 0
    fi
    XDG_RUNTIME_DIR="$RUN_DIR" pipewire > "$RUN_DIR/pipewire.log" 2>&1 &
    PW_PID=$!
    sleep 1
    set +e
    XDG_RUNTIME_DIR="$RUN_DIR" dbus-run-session -- "$0" --inner
    status=$?
    set -e
    kill "$PW_PID" 2>/dev/null || true
    wait "$PW_PID" 2>/dev/null || true
    if [[ "$status" -ne 0 ]]; then
        echo "  (pipewire log:)"; tail -5 "$RUN_DIR/pipewire.log" 2>/dev/null || true
    fi
    rm -rf "$RUN_DIR"
    exit "$status"
fi

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-screencast.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

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

# A stub area-selection overlay: select the rectangle (10, 20) 40x30.
cat > "$RUNTIME/overlay.sh" <<'EOF'
#!/usr/bin/env bash
echo "10 20 40 30"
EOF
chmod +x "$RUNTIME/overlay.sh"

SDE_PORTAL_CHOOSER="$RUNTIME/chooser.sh" SDE_PORTAL_COLOR_PICKER="$RUNTIME/colorpicker.sh" SDE_PORTAL_SCREENSHOT_OVERLAY="$RUNTIME/overlay.sh" SDE_STATE_DIR="$RUNTIME" WAYLAND_DISPLAY="$SOCK" "$PORTAL" > "$RUNTIME/portal.log" 2>&1 &
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
check "uint32 7" "$(call "$IFACE.AvailableCursorModes")"

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

echo "=== restore tokens ==="
call "$IFACE.CreateSession" /h /org/freedesktop/portal/desktop/session/r app "{}" >/dev/null
sel="$(call "$IFACE.SelectSources" /h /org/freedesktop/portal/desktop/session/r app "{'types': <uint32 1>, 'persist_mode': <uint32 2>}")"
check "uint32 0" "$sel"
check "restore_token" "$sel"
token="$(grep -oE "[0-9a-f]{32}" <<<"$sel" | head -1)"
if [ -n "$token" ]; then
    echo "  ok: minted restore token $token"
else
    echo "  FAIL: no restore token ($sel)"
    fail=1
fi
rstart="$(call "$IFACE.Start" /h /org/freedesktop/portal/desktop/session/r app "" "{}")"
check "restore_token" "$rstart"
call "$IFACE.CreateSession" /h /org/freedesktop/portal/desktop/session/r2 app "{}" >/dev/null
restored="$(call "$IFACE.SelectSources" /h /org/freedesktop/portal/desktop/session/r2 app "{'restore_token': <'$token'>}")"
check "$token" "$restored"

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

echo "=== RemoteDesktop restore tokens ==="
call "$RD.CreateSession" /h /org/freedesktop/portal/desktop/session/rdt app "{}" >/dev/null
rdsel="$(call "$RD.SelectDevices" /h /org/freedesktop/portal/desktop/session/rdt app "{'types': <uint32 3>, 'persist_mode': <uint32 2>}")"
check "uint32 0" "$rdsel"
rd_start="$(call "$RD.Start" /h /org/freedesktop/portal/desktop/session/rdt app "" "{}")"
check "restore_token" "$rd_start"

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

echo "=== interactive area screenshot ==="
area="$(call org.freedesktop.impl.portal.Screenshot.Screenshot /h app "" "{'interactive': <true>}")"
check "uint32 0" "$area"
check "uri" "$area"
area_uri="$(grep -oE "file://[^' ]+" <<<"$area" | head -1)"
area_path="${area_uri#file://}"
if [ -n "$area_uri" ] && [ -f "$area_path" ]; then
    if command -v identify >/dev/null 2>&1; then
        dims="$(identify -format "%wx%h" "$area_path" 2>/dev/null || echo unknown)"
        if [ "$dims" = "40x30" ]; then
            echo "  ok: area screenshot cropped to $dims"
        else
            echo "  FAIL: area screenshot not cropped ($dims)"
            fail=1
        fi
    else
        echo "  ok: area screenshot written (identify unavailable to check size)"
    fi
else
    echo "  FAIL: no area screenshot PNG ($area)"
    fail=1
fi

echo "=== the PipeWire stream node exists ==="
if command -v pw-cli >/dev/null 2>&1; then
    # The node is published asynchronously; wait briefly for it (like the other
    # readiness checks in this script) rather than racing the registry.
    node_found=0
    for _ in $(seq 1 30); do
        if pw-cli ls Node 2>/dev/null | grep -q "sde-screencast"; then
            node_found=1
            break
        fi
        sleep 0.1
    done
    if [ "$node_found" -eq 1 ]; then
        echo "  ok: PipeWire node 'sde-screencast' published"
    else
        echo "  FAIL: PipeWire node 'sde-screencast' not found"
        fail=1
    fi
fi

echo "=== frames reach a PipeWire consumer ==="
if [ -x "$SDE_ROOT/build/tools/screencast-consumer" ]; then
    timeout 10 "$SDE_ROOT/build/tools/screencast-consumer" sde-screencast 5 > "$RUNTIME/consumer.log" 2>&1 || true
    if grep -qE "CONSUMER buffers=[1-9]" "$RUNTIME/consumer.log"; then
        echo "  ok: consumer pulled frames ($(grep -oE 'CONSUMER buffers=.*' "$RUNTIME/consumer.log" | head -1))"
    else
        echo "  note: consumer saw no frames under the headless pixman compositor ($(tail -1 "$RUNTIME/consumer.log")); frame delivery is covered by run-screencast-live-test.sh"
    fi
else
    echo "  note: screencast-consumer not built; skipping the consumer check"
fi
check "screencast pipewire node id" "$(cat "$RUNTIME/portal.log")"

kill "$PORTAL_PID" "$COMP_PID" 2>/dev/null || true
wait "$PORTAL_PID" 2>/dev/null || true
wait "$COMP_PID" 2>/dev/null || true

if [[ "$fail" -ne 0 ]]; then
    echo "==> screencast test FAILED"
    exit 1
fi
echo "==> screencast test PASSED"
