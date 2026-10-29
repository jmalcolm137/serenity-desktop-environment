#!/usr/bin/env bash
# Live DMA-BUF ScreenCast test (R5).
#
# Proves zero-copy end-to-end: the portal backend negotiates
# SPA_DATA_DmaBuf with a consumer, allocating GBM buffers the compositor
# renders into. It runs a private PipeWire + WirePlumber (so it can start a
# clean graph) while the backend captures the *live* SDE session via an absolute
# WAYLAND_DISPLAY.
#
# Skips (pass) when there is no WAYLAND_DISPLAY, so CI stays green.
#
# Usage: WAYLAND_DISPLAY=sde scripts/run-screencast-dmabuf-test.sh
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [ "${1:-}" != "--inner" ]; then
    if [[ -z "${WAYLAND_DISPLAY:-}" ]]; then
        echo "note: WAYLAND_DISPLAY is unset; skipping the DMA-BUF test"
        echo "==> screencast DMA-BUF test SKIPPED"
        exit 0
    fi
    # A desktop shell's WAYLAND_DISPLAY is usually the host compositor (KWin, Mutter,
    # ...), which has none of SDE's capture protocols, so the test could only fail
    # there. Run it against an SDE session only: SDE's compositor is the one that
    # advertises serenity_window_manager.
    if command -v wayland-info >/dev/null 2>&1 \
        && ! wayland-info 2>/dev/null | grep -q "'serenity_window_manager'"; then
        echo "note: WAYLAND_DISPLAY=$WAYLAND_DISPLAY is not an SDE session; skipping (point it at one, e.g. WAYLAND_DISPLAY=sde)"
        echo "==> screencast DMA-BUF test SKIPPED"
        exit 0
    fi
    if [[ "$WAYLAND_DISPLAY" != /* ]]; then
        SDE_DMABUF_WAYLAND="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$WAYLAND_DISPLAY"
    else
        SDE_DMABUF_WAYLAND="$WAYLAND_DISPLAY"
    fi
    export SDE_DMABUF_WAYLAND
    RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-dmabuf.XXXXXX")"
    chmod 700 "$RUNTIME"
    export SDE_DMABUF_RUNTIME="$RUNTIME"
    trap 'for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME" 2>/dev/null || true' EXIT
    XDG_RUNTIME_DIR="$RUNTIME" exec dbus-run-session -- "$0" --inner
fi

RUNTIME="${SDE_DMABUF_RUNTIME:?}"
export XDG_RUNTIME_DIR="$RUNTIME"
WAYLAND_SOCK="${SDE_DMABUF_WAYLAND:?}"

"$SDE_ROOT/scripts/build-tools.sh" >/dev/null

command -v wireplumber >/dev/null 2>&1 || {
    echo "note: wireplumber not found; skipping the DMA-BUF test"
    echo "==> screencast DMA-BUF test SKIPPED"
    exit 0
}

pipewire > "$RUNTIME/pipewire.log" 2>&1 &
PW=$!
sleep 1
wireplumber > "$RUNTIME/wireplumber.log" 2>&1 &
WP=$!
sleep 1

printf '#!/usr/bin/env bash\necho 0\n' > "$RUNTIME/chooser.sh"
chmod +x "$RUNTIME/chooser.sh"
WAYLAND_DISPLAY="$WAYLAND_SOCK" SDE_PORTAL_CHOOSER="$RUNTIME/chooser.sh" SDE_STATE_DIR="$RUNTIME" \
    "$SDE_ROOT/build/tools/xdg-desktop-portal-serenity" > "$RUNTIME/portal.log" 2>&1 &
PORTAL=$!
trap 'kill "$PORTAL" "$WP" "$PW" 2>/dev/null || true; for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME" 2>/dev/null || true; rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true' EXIT

DEST="org.freedesktop.impl.portal.desktop.serenity"
OBJ="/org/freedesktop/portal/desktop"
IFACE="org.freedesktop.impl.portal.ScreenCast"
call() { gdbus call --session --dest "$DEST" --object-path "$OBJ" --method "$@" 2>&1; }
for _ in $(seq 1 50); do
    gdbus call --session --dest "$DEST" --object-path "$OBJ" --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1 && break
    sleep 0.1
done

echo "=== ScreenCast session ==="
call "$IFACE.CreateSession" /h /org/freedesktop/portal/desktop/session/a app "{}" >/dev/null
call "$IFACE.SelectSources" /h /org/freedesktop/portal/desktop/session/a app "{'types': <uint32 1>, 'cursor_mode': <uint32 1>}" >/dev/null
start="$(call "$IFACE.Start" /h /org/freedesktop/portal/desktop/session/a app "" "{}")"
if ! grep -q "'streams'" <<<"$start"; then
    echo "FAIL: Start did not return a stream ($start)"
    exit 1
fi
echo "  ok: Start returned a stream"

echo "=== zero-copy DMA-BUF ==="
CONSUMER_DMABUF=1 timeout 10 "$SDE_ROOT/build/tools/screencast-consumer" sde-screencast 5 > "$RUNTIME/consumer.log" 2>&1 || true
cat "$RUNTIME/consumer.log"
if grep -q "zero-copy DMA-BUF buffers" "$RUNTIME/consumer.log"; then
    echo "  ok: consumer received DMA-BUF buffers"
    grep -E "CONSUMER buffers=" "$RUNTIME/consumer.log"
else
    echo "FAIL: consumer did not receive DMA-BUF buffers"
    exit 1
fi
echo "==> screencast DMA-BUF test PASSED"
