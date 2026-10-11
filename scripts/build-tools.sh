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

# sde-dpms (output power for blank-before-lock).
POWER_XML="$WLROOTS_SRC/protocol/wlr-output-power-management-unstable-v1.xml"
wayland-scanner client-header "$POWER_XML" "$TOOLS_BUILD/wlr-output-power-management-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$POWER_XML" "$TOOLS_BUILD/wlr-output-power-management-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/sde-dpms.c" \
    "$TOOLS_BUILD/wlr-output-power-management-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/sde-dpms"

# sde-screenshot (screen capture via wlr-screencopy). The compositor exposes the
# standard ext-image-copy-capture-v1 (and wlr-screencopy compat), so `grim` works
# too; this is the dependency-free helper for scripted/headless screenshots
# (docs/03-engineering-conventions.md D7).
SHOT_XML="$WLROOTS_SRC/protocol/wlr-screencopy-unstable-v1.xml"
wayland-scanner client-header "$SHOT_XML" "$TOOLS_BUILD/wlr-screencopy-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$SHOT_XML" "$TOOLS_BUILD/wlr-screencopy-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/sde-screenshot.c" \
    "$TOOLS_BUILD/wlr-screencopy-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/sde-screenshot"

echo "==> SDE tools: $TOOLS_BUILD/sde-idle $TOOLS_BUILD/sde-dpms $TOOLS_BUILD/sde-screenshot"
