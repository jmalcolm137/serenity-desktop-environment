#!/usr/bin/env bash
# Live camera portal check (R5).
#
# The Camera portal (org.freedesktop.portal.Camera) is implemented by the
# xdg-desktop-portal *frontend* itself (desktop-portal/camera.c): it tracks the
# camera nodes PipeWire exposes and prompts through the Access backend
# (org.freedesktop.impl.portal.Access) and the Lockdown backend (disable-camera).
# There is no camera backend to implement and no device broker to install — the
# only requirements are a PipeWire camera producer (the built-in v4l2 plugin, or
# pipewire-libcamera for libcamera-only cameras) and an Access backend, which
# xdg-desktop-portal-serenity already provides.
#
# This checks the live portal (skips if it is not on the session bus).
set -euo pipefail

PORTAL="org.freedesktop.portal.Desktop"
OBJ="/org/freedesktop/portal/desktop"

if ! command -v gdbus >/dev/null 2>&1; then
    echo "note: gdbus not found; skipping the camera test"
    echo "==> camera test SKIPPED"
    exit 0
fi

if ! gdbus call --session --dest "$PORTAL" --object-path "$OBJ" \
        --method org.freedesktop.DBus.Peer.Ping >/dev/null 2>&1; then
    echo "note: no xdg-desktop-portal frontend on the session bus; skipping"
    echo "==> camera test SKIPPED"
    exit 0
fi

get() {
    gdbus call --session --dest "$PORTAL" --object-path "$OBJ" \
        --method org.freedesktop.DBus.Properties.Get "$@" 2>&1
}

echo "=== org.freedesktop.portal.Camera ==="
version="$(get org.freedesktop.portal.Camera version)"
present="$(get org.freedesktop.portal.Camera IsCameraPresent)"
echo "  version: $version"
echo "  IsCameraPresent: $present"

fail=0
if grep -q "uint32" <<<"$version"; then
    echo "  ok: the Camera portal is exported (an Access impl is available)"
else
    echo "  FAIL: the Camera portal is not exported ($version)"
    fail=1
fi
if grep -qE '<(true|false)>' <<<"$present"; then
    echo "  ok: IsCameraPresent is a boolean"
else
    echo "  FAIL: unexpected IsCameraPresent ($present)"
    fail=1
fi

if grep -q "true" <<<"$present"; then
    echo "  ok: PipeWire exposes camera nodes (AccessCamera/OpenPipeWireRemote are usable)"
else
    echo "  note: no camera nodes in PipeWire (a v4l2/webcam or pipewire-libcamera provider is needed)"
fi

if [[ "$fail" -ne 0 ]]; then
    echo "==> camera test FAILED"
    exit 1
fi
echo "==> camera test PASSED"
