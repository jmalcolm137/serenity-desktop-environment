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
SW_XML="$SDE_ROOT/WindowServer/protocol/serenity-window.xml"
wayland-scanner client-header "$SW_XML" "$TOOLS_BUILD/serenity-window-client-protocol.h"
wayland-scanner private-code  "$SW_XML" "$TOOLS_BUILD/serenity-window-protocol.c"
PRES_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/presentation-time/presentation-time.xml"
wayland-scanner client-header "$PRES_XML" "$TOOLS_BUILD/presentation-time-client-protocol.h"
wayland-scanner private-code  "$PRES_XML" "$TOOLS_BUILD/presentation-time-protocol.c"
FRAC_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/staging/fractional-scale/fractional-scale-v1.xml"
wayland-scanner client-header "$FRAC_XML" "$TOOLS_BUILD/fractional-scale-v1-client-protocol.h"
wayland-scanner private-code  "$FRAC_XML" "$TOOLS_BUILD/fractional-scale-v1-protocol.c"
DECO_XML="$(pkg-config --variable=pkgdatadir wayland-protocols)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml"
wayland-scanner client-header "$DECO_XML" "$TOOLS_BUILD/xdg-decoration-client-protocol.h"
wayland-scanner private-code  "$DECO_XML" "$TOOLS_BUILD/xdg-decoration-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/native-app-test.c" \
    "$TOOLS_BUILD/xdg-shell-protocol.c" \
    "$TOOLS_BUILD/serenity-window-protocol.c" \
    "$TOOLS_BUILD/presentation-time-protocol.c" \
    "$TOOLS_BUILD/fractional-scale-v1-protocol.c" \
    "$TOOLS_BUILD/xdg-decoration-protocol.c" \
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

# output-management-test (R6/N5): disable or re-enable one output through
# zwlr_output_management_v1 — the compositor's hotplug path as a client can drive
# it. Used by run-output-hotplug-test.sh.
OM_XML="$WLROOTS_SRC/protocol/wlr-output-management-unstable-v1.xml"
wayland-scanner client-header "$OM_XML" "$TOOLS_BUILD/wlr-output-management-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$OM_XML" "$TOOLS_BUILD/wlr-output-management-unstable-v1-protocol.c"
cc -O2 -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/output-management-test.c" \
    "$TOOLS_BUILD/wlr-output-management-unstable-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/output-management-test"

# sde-settings-portal (R4.4) and xdg-desktop-portal-serenity (R5): the freedesktop
# portal backends. Serenity-style C++ (AK/LibCore) over the `dbus-1` transport —
# see tools/portal/dbus.h; no GLib/GIO. They link the Lagom userland libraries.
SERENITY_BUILD_DIR="${SERENITY_BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
PORTAL_INCLUDES=(
    -I "$TOOLS_BUILD"
    -I "$SDE_ROOT/tools/portal"
    -I "$SERENITY"
    -I "$SERENITY/Userland"
    -I "$SERENITY/Userland/Libraries"
    -I "$SERENITY/Userland/Services"
    -I "$SERENITY_BUILD_DIR"
    -I "$SERENITY_BUILD_DIR/Userland"
    -I "$SERENITY_BUILD_DIR/Userland/Libraries"
    -I "$SERENITY_BUILD_DIR/Userland/Services"
)
LAGOM_LIBS=(-L "$SERENITY_BUILD_DIR/lib" -llagom-ak -llagom-coreminimal -llagom-corebasic -llagom-core -llagom-threading -llagom-main)
# The main backend decodes/encodes images (wallpaper, screenshots) with LibGfx.
PORTAL_LAGOM_LIBS=("${LAGOM_LIBS[@]}" -llagom-gfx -llagom-compress -llagom-crypto -llagom-textcodec -llagom-unicode -llagom-url -llagom-filesystem -llagom-riff -llagom-ipc)
PORTAL_FLAGS=(-std=c++26 -fno-exceptions -Wno-literal-suffix -O2 -Wall "${PORTAL_INCLUDES[@]}")

c++ "${PORTAL_FLAGS[@]}" \
    $(pkg-config --cflags dbus-1) \
    "$SDE_ROOT/tools/portal/dbus.cpp" \
    "$SDE_ROOT/tools/portal/introspection.cpp" \
    "$SDE_ROOT/tools/portal/settings-portal.cpp" \
    "${LAGOM_LIBS[@]}" $(pkg-config --libs dbus-1) -lpthread \
    -Wl,-rpath,"$SERENITY_BUILD_DIR/lib" \
    -o "$TOOLS_BUILD/sde-settings-portal"

# xdg-desktop-portal-serenity (R5): the SDE backend for the freedesktop portals
# (org.freedesktop.impl.portal.*), so the xdg-desktop-portal frontend can route
# requests to it. See contrib/xdg-desktop-portal/. ScreenCast captures via
# ext-image-copy-capture-v1 and publishes a PipeWire stream.
ICC_XML="$WP/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml"
ICS_XML="$WP/staging/ext-image-capture-source/ext-image-capture-source-v1.xml"
FTL_XML="$WP/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml"
DMABUF_XML="$WP/stable/linux-dmabuf/linux-dmabuf-v1.xml"
VPOINTER_XML="$WLROOTS_SRC/protocol/wlr-virtual-pointer-unstable-v1.xml"
VKEYBOARD_XML="$WLROOTS_SRC/protocol/virtual-keyboard-unstable-v1.xml"
wayland-scanner client-header "$ICC_XML" "$TOOLS_BUILD/ext-image-copy-capture-v1-client-protocol.h"
wayland-scanner private-code  "$ICC_XML" "$TOOLS_BUILD/ext-image-copy-capture-v1-protocol.c"
wayland-scanner client-header "$ICS_XML" "$TOOLS_BUILD/ext-image-capture-source-v1-client-protocol.h"
wayland-scanner private-code  "$ICS_XML" "$TOOLS_BUILD/ext-image-capture-source-v1-protocol.c"
wayland-scanner client-header "$FTL_XML" "$TOOLS_BUILD/ext-foreign-toplevel-list-v1-client-protocol.h"
wayland-scanner private-code  "$FTL_XML" "$TOOLS_BUILD/ext-foreign-toplevel-list-v1-protocol.c"
wayland-scanner client-header "$DMABUF_XML" "$TOOLS_BUILD/linux-dmabuf-v1-client-protocol.h"
wayland-scanner private-code  "$DMABUF_XML" "$TOOLS_BUILD/linux-dmabuf-v1-protocol.c"
wayland-scanner client-header "$VPOINTER_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VPOINTER_XML" "$TOOLS_BUILD/wlr-virtual-pointer-unstable-v1-protocol.c"
wayland-scanner client-header "$VKEYBOARD_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-client-protocol.h"
wayland-scanner private-code  "$VKEYBOARD_XML" "$TOOLS_BUILD/virtual-keyboard-unstable-v1-protocol.c"
# serenity-input-capture: the private protocol for the InputCapture portal
# (docs/17-input-capture.md): session control + the compositor's EIS fd.
IC_XML="$SDE_ROOT/WindowServer/protocol/serenity-input-capture.xml"
wayland-scanner client-header "$IC_XML" "$TOOLS_BUILD/serenity-input-capture-client-protocol.h"
wayland-scanner private-code  "$IC_XML" "$TOOLS_BUILD/serenity-input-capture-protocol.c"

# The generated protocol code is C; compile it with the C compiler.
PORTAL_PROTOCOLS=(
    ext-image-copy-capture-v1-protocol
    ext-image-capture-source-v1-protocol
    ext-foreign-toplevel-list-v1-protocol
    linux-dmabuf-v1-protocol
    wlr-virtual-pointer-unstable-v1-protocol
    virtual-keyboard-unstable-v1-protocol
    serenity-input-capture-protocol
)
for protocol in "${PORTAL_PROTOCOLS[@]}"; do
    cc -O2 -I "$TOOLS_BUILD" $(pkg-config --cflags wayland-client) \
        -c "$TOOLS_BUILD/$protocol.c" -o "$TOOLS_BUILD/$protocol.o"
done

PORTAL_OBJECTS=()
for protocol in "${PORTAL_PROTOCOLS[@]}"; do
    PORTAL_OBJECTS+=("$TOOLS_BUILD/$protocol.o")
done

c++ "${PORTAL_FLAGS[@]}" \
    $(pkg-config --cflags dbus-1 wayland-client libpipewire-0.3 xkbcommon gbm libdrm) \
    "$SDE_ROOT/tools/portal/dbus.cpp" \
    "$SDE_ROOT/tools/portal/introspection.cpp" \
    "$SDE_ROOT/tools/portal/host.cpp" \
    "$SDE_ROOT/tools/portal/backend.cpp" \
    "$SDE_ROOT/tools/portal/screencast.cpp" \
    "$SDE_ROOT/tools/portal/remote-desktop.cpp" \
    "$SDE_ROOT/tools/portal/input-capture.cpp" \
    "$SDE_ROOT/tools/portal/global-shortcuts.cpp" \
    "$SDE_ROOT/tools/portal/usb.cpp" \
    "${PORTAL_OBJECTS[@]}" \
    "${PORTAL_LAGOM_LIBS[@]}" \
    $(pkg-config --libs dbus-1 wayland-client libpipewire-0.3 xkbcommon gbm libdrm) \
    -lpthread -Wl,-rpath,"$SERENITY_BUILD_DIR/lib" \
    -o "$TOOLS_BUILD/xdg-desktop-portal-serenity"

# capture-probe (R5): reports what the live compositor can offer the ScreenCast
# portal — DMA-BUF capture formats, a real DMA-BUF frame, and the pointer-cursor
# session. The live counterpart to run-screencast-test.sh (docs/15 §6).
cc -O2 -Wall -Wextra -I "$TOOLS_BUILD" \
    "$SDE_ROOT/tools/capture-probe.c" \
    "$TOOLS_BUILD/ext-image-copy-capture-v1-protocol.c" \
    "$TOOLS_BUILD/ext-image-capture-source-v1-protocol.c" \
    "$TOOLS_BUILD/ext-foreign-toplevel-list-v1-protocol.c" \
    "$TOOLS_BUILD/linux-dmabuf-v1-protocol.c" \
    $(pkg-config --cflags --libs wayland-client gbm libdrm) \
    -o "$TOOLS_BUILD/capture-probe"

# screencast-consumer (R5): connects to the ScreenCast PipeWire node and reports
# the delivered buffer type (DMA-BUF vs memfd) and cursor metadata. The
# verification tool for docs/15 §6.2/§6.3.
cc -O2 -Wall -Wextra \
    "$SDE_ROOT/tools/screencast-consumer.c" \
    $(pkg-config --cflags --libs libpipewire-0.3 libdrm) \
    -o "$TOOLS_BUILD/screencast-consumer"

echo "==> SDE tools: $TOOLS_BUILD/output-management-test $TOOLS_BUILD/sde-idle $TOOLS_BUILD/sde-dpms $TOOLS_BUILD/sde-screenshot $TOOLS_BUILD/sde-settings-portal $TOOLS_BUILD/ext-workspace-test $TOOLS_BUILD/native-app-test $TOOLS_BUILD/capture-probe $TOOLS_BUILD/screencast-consumer $TOOLS_BUILD/xdg-desktop-portal-serenity"
