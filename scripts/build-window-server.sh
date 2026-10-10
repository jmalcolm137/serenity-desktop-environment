#!/usr/bin/env bash
# Build serenity-window-server.
#
# It links the SerenityOS userland libraries (AK, LibCore, ...) from the
# libgui-wayland build and wlroots from third_party/wlroots.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
BUILD_DIR="${BUILD_DIR:-$SDE_ROOT/build/window-server}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "$WLROOTS_SRC/install/lib/pkgconfig/wlroots-0.20.pc" ]]; then
    "$SDE_ROOT/scripts/fetch-wlroots.sh"
fi

if [[ ! -f "$SERENITY_BUILD_DIR/lib/liblagom-ak.so" ]]; then
    echo "error: Serenity userland libraries not found in $SERENITY_BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

export PKG_CONFIG_PATH="$WLROOTS_SRC/install/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

cmake -S "$SDE_ROOT/window-server" -B "$BUILD_DIR" -GNinja \
    -DSERENITY_SRC="$SERENITY_SRC" \
    -DSERENITY_BUILD_DIR="$SERENITY_BUILD_DIR" \
    -DWLROOTS_PROTOCOL_INCLUDE_DIR="$WLROOTS_SRC/build/protocol"

ninja -C "$BUILD_DIR" -j"$JOBS"
echo "==> serenity-window-server: $BUILD_DIR/serenity-window-server"
