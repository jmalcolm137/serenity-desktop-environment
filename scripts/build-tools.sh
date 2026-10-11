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

# native-app-test (R1/R4): a minimal xdg-shell client, for scripted toplevels
# (used by the headless and screencast tests).
XDG_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml"
wayland-scanner client-header "$XDG_XML" "$TOOLS_BUILD/xdg-shell-client-protocol.h"
wayland-scanner private-code  "$XDG_XML" "$TOOLS_BUILD/xdg-shell-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/native-app-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/native-app-test"

# ext-workspace-test (R4.8): a scripted ext-workspace-v1 client, so the standard
# workspace protocol can be exercised without waybar.
WS_XML="$WP/staging/ext-workspace/ext-workspace-v1.xml"
wayland-scanner client-header "$WS_XML" "$TOOLS_BUILD/ext-workspace-v1-client-protocol.h"
wayland-scanner private-code  "$WS_XML" "$TOOLS_BUILD/ext-workspace-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/ext-workspace-test.c" \
    "$TOOLS_BUILD/ext-workspace-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/ext-workspace-test"

# sde-settings-portal (R4.4): the SDE backend for the freedesktop Settings portal
# (org.freedesktop.impl.portal.Settings) — reports color-scheme / accent-color /
# contrast from the active SDE theme, so GTK/Qt/Flatpak apps follow it. Host
# D-Bus via GIO.
cc -O2 -Wall \
    "$SDE_ROOT/tools/sde-settings-portal.c" \
    $(pkg-config --cflags --libs gio-2.0) \
    -o "$TOOLS_BUILD/sde-settings-portal"

# xdg-desktop-portal-serenity (R5): the SDE backend for the freedesktop portals
# (org.freedesktop.impl.portal.*), so the xdg-desktop-portal frontend can route
# requests to it. See contrib/xdg-desktop-portal/. ScreenCast captures via
# ext-image-copy-capture-v1 and publishes a PipeWire stream.
ICC_XML="$WP/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml"
ICS_XML="$WP/staging/ext-image-capture-source/ext-image-capture-source-v1.xml"
FTL_XML="$WP/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
VPOINTER_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
VKEYBOARD_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
wayland-scanner client-header "$ICC_XML" "$TOOLS_BUILD/ext-image-copy-capture-v1-client-protocol.h"
wayland-scanner private-code  "$ICC_XML" "$TOOLS_BUILD/ext-image-copy-capture-v1-protocol.c"
wayland-scanner client-header "$ICS_XML" "$TOOLS_BUILD/ext-image-capture-source-v1-client-protocol.h"
wayland-scanner private-code  "$ICS_XML" "$TOOLS_BUILD/ext-image-capture-source-v1-protocol.c"
wayland-scanner client-header "$FTL_XML" "$TOOLS_BUILD/ext-foreign-toplevel-list-v1-client-protocol.h"
wayland-scanner private-code  "$FTL_XML" "$TOOLS_BUILD/ext-foreign-toplevel-list-v1-protocol.c"
wayland-scanner client-header "$VPOINTER_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VPOINTER_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
wayland-scanner client-header "$VKEYBOARD_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VKEYBOARD_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
cc -O2 -Wall -I "$TOOLS_BUILD" -I "$SDE_ROOT/tools" \
    "$SDE_ROOT/tools/xdg-desktop-portal-serenity.c" \
    "$SDE_ROOT/tools/portal-screencast.c" \
    "$SDE_ROOT/tools/portal-remote-desktop.c" \
    "$TOOLS_BUILD/ext-image-copy-capture-v1-protocol.c" \
    "$TOOLS_BUILD/ext-image-capture-source-v1-protocol.c" \
    "$TOOLS_BUILD/ext-foreign-toplevel-list-v1-protocol.c" \
    "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c" \
    "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs gio-2.0 wayland-client libpipewire-0.3 gdk-pixbuf-2.0 xkbcommon) \
    -lpthread \
    -o "$TOOLS_BUILD/xdg-desktop-portal-serenity"

echo "==> SDE tools: $TOOLS_BUILD/sde-idle $TOOLS_BUILD/sde-dpms $TOOLS_BUILD/sde-screenshot $TOOLS_BUILD/sde-settings-portal $TOOLS_BUILD/ext-workspace-test $TOOLS_BUILD/native-app-test $TOOLS_BUILD/xdg-desktop-portal-serenity"
