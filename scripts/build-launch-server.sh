#!/usr/bin/env bash
# Build the SDE session LaunchServer (see docs/06-service-layering.md).
#
# It links the SerenityOS userland libraries and libgui-wayland's
# LaunchServerConnection from the libgui-wayland build.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
BUILD_DIR="${BUILD_DIR:-$SDE_ROOT/build/LaunchServer}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "$SERENITY_BUILD_DIR/lib/liblagom-ak.so" ]]; then
    echo "error: Serenity userland libraries not found in $SERENITY_BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

cmake -S "$SDE_ROOT/LaunchServer" -B "$BUILD_DIR" -GNinja \
    -DSERENITY_SRC="$SERENITY_SRC" \
    -DSERENITY_BUILD_DIR="$SERENITY_BUILD_DIR" \
    -DLIBGUI_WAYLAND_SRC="$LIBGUI_WAYLAND_ROOT"

ninja -C "$BUILD_DIR" -j"$JOBS"
echo "==> LaunchServer: $BUILD_DIR/LaunchServer"
