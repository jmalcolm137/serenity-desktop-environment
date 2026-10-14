#!/usr/bin/env bash
# Build the SDE's audio applications.
#
# Serenity applications that speak the AudioServer / AudioManager protocols
# directly belong with the SDE's AudioServer service, not in the
# compositor/desktop-agnostic client stack (libgui-wayland builds those targets
# but does not build these by default):
#
#   Piano       - a live audio client (Audio::ConnectionToServer)
#   SoundPlayer - a media player and mixer UI (audio + audiomanager clients)
#
# They are unmodified SerenityOS source; this requests their build from the
# shared Lagom build, the same way the session services are built. With the
# AudioServer now an out-of-process service, these apps connect to it over the
# session portal rather than embedding a shim.
#
# See docs/06-service-layering.md and docs/07-systemserver-roles.md.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD_DIR="${BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
JOBS="${JOBS:-$(nproc)}"

APPS=(Piano SoundPlayer)

if [[ ! -e "$BUILD_DIR/bin/Piano" && ! -e "$BUILD_DIR/lib/liblagom-gui.so" ]]; then
    echo "error: the Serenity userland build was not found in $BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

for app in "${APPS[@]}"; do
    echo "==> Building $app"
    ninja -C "$BUILD_DIR" -j"$JOBS" "$app"
done
