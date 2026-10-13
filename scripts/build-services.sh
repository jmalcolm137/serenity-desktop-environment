#!/usr/bin/env bash
# Build the SDE session's out-of-process services.
#
# These are the Serenity services a session needs that are not part of a
# compositor-agnostic client stack, so SDE owns building them (libgui-wayland
# keeps only its own in-process/standalone fallback):
#
#   WebContent         - the WebView renderer (Browser, Mail, Help, ...)
#   RequestServer      - the HTTP/TLS backend for page loads
#   SQLServer          - Browser history/bookmarks/cookies
#   NotificationServer - the notify portal (notifications)
#
# LibWM spawns them by name from the session app directory (SDE_APP_BIN_DIR) the
# first time an application asks; the session assembles that directory from these
# binaries in scripts/sde-session.sh. They are not build dependencies of the apps
# that use them, so building the apps does not build them.
#
# The services that LibWM serves in-process (clipboard, config, launch,
# filesystem-access) are deliberately not here; nor are the ones the session
# builds elsewhere (LaunchServer, ImageDecoder, the portal backend).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
JOBS="${JOBS:-$(nproc)}"

SERVICES=(WebContent RequestServer SQLServer NotificationServer)

if [[ ! -f "$SERENITY_BUILD_DIR/lib/liblagom-ak.so" ]]; then
    echo "error: Serenity userland libraries not found in $SERENITY_BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

for service in "${SERVICES[@]}"; do
    BUILD_DIR="$SDE_ROOT/build/$service"
    echo "==> Building $service"
    cmake -S "$SDE_ROOT/$service" -B "$BUILD_DIR" -GNinja \
        -DSERENITY_SRC="$SERENITY_SRC" \
        -DSERENITY_BUILD_DIR="$SERENITY_BUILD_DIR" \
        -DLIBGUI_WAYLAND_SRC="$LIBGUI_WAYLAND_ROOT"
    ninja -C "$BUILD_DIR" -j"$JOBS"
    echo "==> $service: $BUILD_DIR/$service"
done
