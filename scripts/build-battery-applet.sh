#!/usr/bin/env bash
# Build BatteryApplet, the SDE battery status applet for the Taskbar's applet
# area (a Serenity LibGUI app; see docs/16-battery-applet.md).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
BUILD_DIR="${BUILD_DIR:-$SDE_ROOT/build/BatteryApplet}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "$SERENITY_BUILD_DIR/lib/liblagom-gui.so" ]]; then
    echo "error: Serenity userland libraries not found in $SERENITY_BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

cmake -S "$SDE_ROOT/BatteryApplet" -B "$BUILD_DIR" -GNinja \
    -DSERENITY_SRC="$SERENITY_SRC" \
    -DSERENITY_BUILD_DIR="$SERENITY_BUILD_DIR" \
    -DLIBGUI_WAYLAND_SRC="$LIBGUI_WAYLAND_ROOT"

ninja -C "$BUILD_DIR" -j"$JOBS"
echo "==> BatteryApplet: $BUILD_DIR/BatteryApplet"
