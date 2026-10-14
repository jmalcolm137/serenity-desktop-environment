#!/usr/bin/env bash
# Build the SDE's test clients (test/).
#
# These are the LibGUI/LibWindowServer programs that drive this desktop's
# integration tests (the notify/tooltip portals, the session app directory, the
# font decision, service layering, the Profiler/SystemMonitor host backends).
# They are the SDE's, so they build here rather than in libgui-wayland; see
# docs/24-libgui-wayland-sde-boundary.md.
#
# With no arguments every test is built; pass target names to build a subset
# (e.g. `scripts/build-tests.sh libwm-notify-test`).
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
BUILD_DIR="${BUILD_DIR:-$SDE_ROOT/build/test}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -e "$SERENITY_BUILD_DIR/lib/liblagom-gui.so" ]]; then
    echo "error: Serenity userland libraries not found in $SERENITY_BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

cmake -S "$SDE_ROOT/test" -B "$BUILD_DIR" -GNinja \
    -DSERENITY_SRC="$SERENITY_SRC" \
    -DSERENITY_BUILD_DIR="$SERENITY_BUILD_DIR" \
    -DLIBGUI_WAYLAND_SRC="$LIBGUI_WAYLAND_ROOT" >/dev/null
ninja -C "$BUILD_DIR" -j"$JOBS" "$@"
