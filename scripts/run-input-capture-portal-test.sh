#!/usr/bin/env bash
# Headless InputCapture *portal* test (P4, docs/17-input-capture.md).
#
# Starts an SDE compositor with the private serenity_input_capture protocol
# (SDE_INPUT_CAPTURE=1) and the xdg-desktop-portal-serenity backend (as a Wayland
# client of that compositor) on a private session bus, then drives
# org.freedesktop.impl.portal.InputCapture end to end:
#   CreateSession2 -> Start (prompt) -> GetZones -> SetPointerBarriers ->
#   ConnectToEIS -> Enable -> [Activated + captured input] -> Disable.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
PORTAL="$SDE_ROOT/build/tools/xdg-desktop-portal-serenity"
VT_BUILD="$SDE_ROOT/build/input-capture-portal-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

# virtual-input-test (to drive a barrier crossing and a key press).
mkdir -p "$VT_BUILD"
VK_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
VP_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
wayland-scanner client-header "$VK_XML" "$VT_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VK_XML" "$VT_BUILD/virtual-keyboard-unstable-v1-protocol.c"
wayland-scanner client-header "$VP_XML" "$VT_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VP_XML" "$VT_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
cc -O2 -I "$VT_BUILD" \
    "$SDE_ROOT/tools/virtual-input-test.c" \
    "$VT_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    "$VT_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client xkbcommon) \
    -o "$VT_BUILD/virtual-input-test"

if [ "${1:-}" != "--inner" ]; then
    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-icportal.XXXXXX")"
    trap 'rm -rf "$RUNTIME"' EXIT
    export SDE_PORTAL_TEST_RUNTIME="$RUNTIME"
    exec dbus-run-session -- "$0" --inner
fi

RUNTIME="$SDE_PORTAL_TEST_RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"
DEST="org.freedesktop.impl.portal.desktop.serenity"
OBJ="/org/freedesktop/portal/desktop"
SESH="/org/freedesktop/portal/desktop/session/1/t"
REQ="/org/freedesktop/portal/desktop/request/1/t"

ALLOW="$RUNTIME/allow.sh"
cat > "$ALLOW" <<EOF
#!/usr/bin/env bash
echo invoke \$@ >> "$RUNTIME/prompts.log"
echo 0
EOF
chmod +x "$ALLOW"

SOCK="sde-icportal-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_INPUT_CAPTURE=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
PORTAL_PID=""
cleanup() {
    [[ -n "$PORTAL_PID" ]] && kill "$PORTAL_PID" 2>/dev/null || true
    kill "$COMP_PID" 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.3

env WAYLAND_DISPLAY="$SOCK" SDE_STATE_DIR="$RUNTIME" \
    SDE_PORTAL_ACCESS_DIALOG="$ALLOW" \
    "$PORTAL" > "$RUNTIME/portal.log" 2>&1 &
PORTAL_PID=$!
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
prop() { call org.freedesktop.DBus.Properties.Get org.freedesktop.impl.portal.InputCapture "$1"; }

echo "=== InputCapture properties ==="
check "uint32 3" "$(prop SupportedCapabilities)"
check "uint32 2" "$(prop version)"

echo "=== session + start ==="
check "@a{sv} {}" "$(call org.freedesktop.impl.portal.InputCapture.CreateSession2 "$SESH" "org.sde.Test" "{}")"
start="$(call org.freedesktop.impl.portal.InputCapture.Start "$REQ" "$SESH" "org.sde.Test" "" "{'capabilities': <uint32 3>}")"
check "capabilities" "$start"
check "uint32 3" "$start"

echo "=== zones ==="
zones="$(call org.freedesktop.impl.portal.InputCapture.GetZones "$REQ" "$SESH" "org.sde.Test" "{}")"
check "uint32 1280, uint32 720" "$zones"

echo "=== barriers (one valid, one diagonal rejected) ==="
barriers="$(call org.freedesktop.impl.portal.InputCapture.SetPointerBarriers "$REQ" "$SESH" "org.sde.Test" "{}" \
    "[{'barrier_id': <uint32 1>, 'position': <(640, 0, 640, 719)>}, {'barrier_id': <uint32 2>, 'position': <(0, 0, 100, 100)>}]" 1)"
check "failed_barriers" "$barriers"
check "<[uint32 2]>" "$barriers"

echo "=== ConnectToEIS ==="
eis="$(call org.freedesktop.impl.portal.InputCapture.ConnectToEIS "$SESH" "org.sde.Test" "{}")"
echo "  ConnectToEIS -> $eis"
check "handed an EIS fd" "$(cat "$RUNTIME/comp.log")"

echo "=== enable + activation ==="
enable="$(call org.freedesktop.impl.portal.InputCapture.Enable "$SESH" "org.sde.Test" "{}")"
check "uint32 0" "$enable"
check "armed" "$(cat "$RUNTIME/comp.log")"

timeout 6 gdbus monitor --session --dest "$DEST" --object-path "$OBJ" > "$RUNTIME/monitor.log" 2>&1 &
MON_PID=$!
sleep 0.6
env WAYLAND_DISPLAY="$SOCK" "$VT_BUILD/virtual-input-test" \
    --extent 1280 720 --move 500 360 --move 800 360 --key 30 press --key 30 release \
    > "$RUNTIME/input.log" 2>&1 || true
for _ in $(seq 1 20); do
    grep -qF "Activated" "$RUNTIME/monitor.log" 2>/dev/null && break
    sleep 0.1
done
kill "$MON_PID" 2>/dev/null || true
check "Activated" "$(cat "$RUNTIME/monitor.log")"
check "activation_id" "$(cat "$RUNTIME/monitor.log")"
check "barrier_id" "$(cat "$RUNTIME/monitor.log")"

echo "=== disable ==="
check "uint32 0" "$(call org.freedesktop.impl.portal.InputCapture.Disable "$SESH" "org.sde.Test" "{}")"
check "InputCapture: session 'xdg-desktop-portal-serenity' disabled" "$(cat "$RUNTIME/comp.log")"

echo "=== P5: persistence (persist_mode / restore_data) ==="
SESH2="/org/freedesktop/portal/desktop/session/1/persist"
REQ2="/org/freedesktop/portal/desktop/request/1/persist"
call org.freedesktop.impl.portal.InputCapture.CreateSession2 "$SESH2" "org.sde.Test" "{}" >/dev/null
before_persist="$(wc -l < "$RUNTIME/prompts.log" 2>/dev/null || echo 0)"
persist="$(call org.freedesktop.impl.portal.InputCapture.Start "$REQ2" "$SESH2" "org.sde.Test" "" "{'capabilities': <uint32 3>, 'persist_mode': <uint32 2>}")"
check "restore_data" "$persist"
check "'SDE'" "$persist"
token="$(grep -oE '[0-9a-f]{32}' <<<"$persist" | head -1)"
if [[ -n "$token" ]]; then
    echo "  ok: persist_mode returned a restore token"
else
    echo "  MISSING: restore token in '$persist'"
    fail=1
fi
after_persist="$(wc -l < "$RUNTIME/prompts.log" 2>/dev/null || echo 0)"

SESH3="/org/freedesktop/portal/desktop/session/1/restore"
REQ3="/org/freedesktop/portal/desktop/request/1/restore"
call org.freedesktop.impl.portal.InputCapture.CreateSession2 "$SESH3" "org.sde.Test" "{}" >/dev/null
restore="$(call org.freedesktop.impl.portal.InputCapture.Start "$REQ3" "$SESH3" "org.sde.Test" "" "{'capabilities': <uint32 3>, 'restore_data': <('SDE', uint32 1, <'$token'>)>}")"
check "restore_data" "$restore"
after_restore="$(wc -l < "$RUNTIME/prompts.log" 2>/dev/null || echo 0)"
if [[ "$after_persist" -gt "$before_persist" ]]; then
    echo "  ok: persist_mode start prompted ($before_persist -> $after_persist)"
else
    echo "  MISSING: persist_mode start did not prompt"
    fail=1
fi
if [[ "$after_restore" == "$after_persist" ]]; then
    echo "  ok: restore_data suppressed the prompt ($after_restore)"
else
    echo "  MISSING: restore_data did not suppress the prompt ($after_persist -> $after_restore)"
    fail=1
fi
if [[ -f "$RUNTIME/screencast-tokens" ]]; then
    echo "  ok: restore token persisted to disk"
else
    echo "  MISSING: restore token file"
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> InputCapture portal test FAILED"
    echo "--- compositor log ---"; cat "$RUNTIME/comp.log"
    echo "--- portal log ---"; cat "$RUNTIME/portal.log"
    exit 1
fi
echo "==> InputCapture portal test PASSED"
