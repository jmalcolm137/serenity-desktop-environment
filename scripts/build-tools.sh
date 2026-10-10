#!/usr/bin/env bash
# Build the portable SDE userland tools (R4.2/…). These are plain Wayland
# clients/services — standard protocols only — so they run under any compositor
# (P8/P9). Currently: sde-idle, the idle-action daemon.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
TOOLS_BUILD="$SDE_ROOT/build/tools"

mkdir -p "$TOOLS_BUILD"
WP="$(pkg-config --variable=pkgdatadir wayland-protocols)"
IDLE_XML="$WP/staging/ext-idle-notify/ext-idle-notify-v1.xml"

wayland-scanner client-header "$IDLE_XML" "$TOOLS_BUILD/ext-idle-notify-v1-client-protocol.h"
wayland-scanner private-code  "$IDLE_XML" "$TOOLS_BUILD/ext-idle-notify-v1-protocol.c"

cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/sde-idle.c" \
    "$TOOLS_BUILD/ext-idle-notify-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/sde-idle"

echo "==> SDE tools: $TOOLS_BUILD/sde-idle"
