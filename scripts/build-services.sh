#!/usr/bin/env bash
# Build the out-of-process Serenity services that LibWM spawns on demand.
#
# These are separate executables with no link-time edge to the applications that
# use them: a WebView app (Browser, Mail, Help, ...) reaches the renderer and the
# network backend through the Serenity portals, and LibWM spawns the matching
# binary from $SDE_APP_BIN_DIR the first time it is asked. Because they are not
# build dependencies of any app, building the apps does not build them -- this
# script does.
#
#   WebContent         - the WebView renderer (Browser, Mail, Help, ...)
#   RequestServer      - the HTTP/TLS backend for page loads
#   SQLServer          - Browser history/bookmarks/cookies
#   NotificationServer - the notify portal (notifications)
#
# The services that LibWM serves in-process (clipboard, config, launch,
# filesystem-access) are deliberately not here; nor are the ones the SDE session
# owns (LaunchServer, ImageDecoder, the portal backend).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
JOBS="${JOBS:-$(nproc)}"

# The Lagom targets are defined by libgui-wayland's build wiring
# (libgui-wayland/src/CMakeLists.txt), so they live in the same build directory
# as the userland libraries and the applications.
SERVICES=(WebContent RequestServer SQLServer NotificationServer)

if [[ ! -f "$SERENITY_BUILD_DIR/lib/liblagom-ak.so" ]]; then
    echo "error: Serenity userland libraries not found in $SERENITY_BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

ninja -C "$SERENITY_BUILD_DIR" -j"$JOBS" "${SERVICES[@]}"
echo "==> SDE session services: ${SERVICES[*]}"
