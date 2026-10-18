# The SDE's userland tools and test clients, built by the top-level CMake build.
#
# The portable tools are plain Wayland clients/services on standard protocols, so
# they run under any compositor (P8/P9): sde-idle, sde-dpms, sde-screenshot, and
# the portal backends (sde-settings-portal, xdg-desktop-portal-serenity). The rest
# are clients the tests drive: native-app-test, ext-workspace-test,
# output-management-test, capture-probe, screencast-consumer and
# virtual-input-test.
#
# Everything lands in build/tools, where the scripts look, except
# virtual-input-test, which the tests find in build/input-test.

include("${CMAKE_SOURCE_DIR}/cmake/SDEWayland.cmake")

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)

set(WP "${WAYLAND_PROTOCOLS_DATADIR}")
set(WLR_PROTOCOLS "${SDE_WLROOTS_SRC}/protocol")
set(SDE_PROTOCOLS "${CMAKE_SOURCE_DIR}/WindowServer/protocol")

# The C clients were always built with -O2 by the scripts; keep that.
function(sde_c_tool name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LIBRARIES;OPTIONS")
    add_executable(${name} ${ARG_SOURCES})
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_options(${name} PRIVATE -O2 ${ARG_OPTIONS})
    target_link_libraries(${name} PRIVATE PkgConfig::WAYLAND_CLIENT ${ARG_LIBRARIES})
endfunction()

# sde-idle: the idle-action daemon (ext-idle-notify-v1).
set(protocols)
sde_wayland_client_protocol(protocols NAME ext-idle-notify-v1 XML "${WP}/staging/ext-idle-notify/ext-idle-notify-v1.xml")
sde_c_tool(sde-idle SOURCES sde-idle.c ${protocols})

# sde-dpms: output power for blank-before-lock.
set(protocols)
sde_wayland_client_protocol(protocols NAME wlr-output-power-management-unstable-v1 XML "${WLR_PROTOCOLS}/wlr-output-power-management-unstable-v1.xml")
sde_c_tool(sde-dpms SOURCES sde-dpms.c ${protocols})

# sde-screenshot: scripted/headless screenshots via wlr-screencopy (the compositor
# also exposes ext-image-copy-capture-v1, so `grim` works too; docs/03 D7).
set(protocols)
sde_wayland_client_protocol(protocols NAME wlr-screencopy-unstable-v1 XML "${WLR_PROTOCOLS}/wlr-screencopy-unstable-v1.xml")
sde_c_tool(sde-screenshot SOURCES sde-screenshot.c ${protocols})

# native-app-test (R1/R4): a minimal xdg-shell client for scripted toplevels.
set(protocols)
sde_wayland_client_protocol(protocols NAME xdg-shell XML "${WP}/stable/xdg-shell/xdg-shell.xml")
sde_wayland_client_protocol(protocols NAME serenity-window XML "${SDE_PROTOCOLS}/serenity-window.xml")
sde_wayland_client_protocol(protocols NAME presentation-time XML "${WP}/stable/presentation-time/presentation-time.xml")
sde_wayland_client_protocol(protocols NAME fractional-scale-v1 XML "${WP}/staging/fractional-scale/fractional-scale-v1.xml")
sde_wayland_client_protocol(protocols NAME xdg-decoration XML "${WP}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml")
sde_c_tool(native-app-test SOURCES native-app-test.c ${protocols})

# wayland-fuzz-test (R6/N8): valid-but-extreme xdg-shell requests, for the
# protocol fuzz harness (run-wayland-fuzz-test.sh).
set(protocols)
sde_wayland_client_protocol(protocols NAME xdg-shell XML "${WP}/stable/xdg-shell/xdg-shell.xml")
sde_c_tool(wayland-fuzz-test SOURCES wayland-fuzz-test.c ${protocols})

# ext-workspace-test (R4.8): a scripted ext-workspace-v1 client.
set(protocols)
sde_wayland_client_protocol(protocols NAME ext-workspace-v1 XML "${WP}/staging/ext-workspace/ext-workspace-v1.xml")
sde_c_tool(ext-workspace-test SOURCES ext-workspace-test.c ${protocols})

# output-management-test (R6/N5): disable or re-enable an output through
# zwlr_output_management_v1 (run-output-hotplug-test.sh).
set(protocols)
sde_wayland_client_protocol(protocols NAME wlr-output-management-unstable-v1 XML "${WLR_PROTOCOLS}/wlr-output-management-unstable-v1.xml")
sde_c_tool(output-management-test SOURCES output-management-test.c ${protocols})

# The capture protocols, shared by the ScreenCast backend and capture-probe.
set(capture_protocols)
sde_wayland_client_protocol(capture_protocols NAME ext-image-copy-capture-v1 XML "${WP}/staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml")
sde_wayland_client_protocol(capture_protocols NAME ext-image-capture-source-v1 XML "${WP}/staging/ext-image-capture-source/ext-image-capture-source-v1.xml")
sde_wayland_client_protocol(capture_protocols NAME ext-foreign-toplevel-list-v1 XML "${WP}/staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml")
sde_wayland_client_protocol(capture_protocols NAME linux-dmabuf-v1 XML "${WP}/stable/linux-dmabuf/linux-dmabuf-v1.xml")

# capture-probe (R5): what the live compositor can offer the ScreenCast portal
# (DMA-BUF formats, a real DMA-BUF frame, the cursor session; docs/15 §6).
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
sde_c_tool(capture-probe
    SOURCES capture-probe.c ${capture_protocols}
    LIBRARIES PkgConfig::GBM PkgConfig::LIBDRM
    OPTIONS -Wall -Wextra)

# screencast-consumer (R5): reads the ScreenCast PipeWire node and reports the
# buffer type and cursor metadata (docs/15 §6.2/§6.3).
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
add_executable(screencast-consumer screencast-consumer.c)
target_compile_options(screencast-consumer PRIVATE -O2 -Wall -Wextra)
target_link_libraries(screencast-consumer PRIVATE PkgConfig::PIPEWIRE PkgConfig::LIBDRM)

# virtual-input-test: scripted keyboard/pointer/touch/tablet input for the
# headless tests (virtual-keyboard, virtual-pointer, serenity-virtual-input).
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
set(protocols)
sde_wayland_client_protocol(protocols NAME virtual-keyboard-unstable-v1 XML "${WLR_PROTOCOLS}/virtual-keyboard-unstable-v1.xml")
sde_wayland_client_protocol(protocols NAME wlr-virtual-pointer-unstable-v1 XML "${WLR_PROTOCOLS}/wlr-virtual-pointer-unstable-v1.xml")
sde_wayland_client_protocol(protocols NAME serenity-virtual-input XML "${SDE_PROTOCOLS}/serenity-virtual-input.xml")
sde_c_tool(virtual-input-test
    SOURCES virtual-input-test.c ${protocols}
    LIBRARIES PkgConfig::XKBCOMMON
    OPTIONS -DSDE_HAVE_VIRTUAL_INPUT)
set_target_properties(virtual-input-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/input-test")

# The portal backends (R4.4, R5): Serenity-style C++ (AK/LibCore) over the
# `dbus-1` transport (tools/portal/dbus.h; no GLib/GIO), linking the Lagom
# userland libraries.
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
function(sde_portal_tool name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;LAGOM;LIBRARIES")
    add_executable(${name} ${ARG_SOURCES})
    set_target_properties(${name} PROPERTIES CXX_STANDARD 26 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    target_compile_options(${name} PRIVATE -fno-exceptions -Wno-literal-suffix -O2 -Wall)
    target_include_directories(${name} PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/portal"
        "${SERENITY_SRC}"
        "${SERENITY_SRC}/Userland"
        "${SERENITY_SRC}/Userland/Libraries"
        "${SERENITY_SRC}/Userland/Services"
        "${SERENITY_BUILD_DIR}"
        "${SERENITY_BUILD_DIR}/Userland"
        "${SERENITY_BUILD_DIR}/Userland/Libraries"
        "${SERENITY_BUILD_DIR}/Userland/Services")
    set(lagom)
    foreach(lib IN LISTS ARG_LAGOM)
        list(APPEND lagom "${LAGOM_${lib}}")
    endforeach()
    target_link_libraries(${name} PRIVATE ${lagom} PkgConfig::DBUS Threads::Threads ${ARG_LIBRARIES})
    set_target_properties(${name} PROPERTIES BUILD_RPATH "${SERENITY_BUILD_DIR}/lib")
endfunction()
find_package(Threads REQUIRED)

set(PORTAL_LAGOM ak coreminimal corebasic core threading main)
sde_portal_tool(sde-settings-portal
    SOURCES portal/dbus.cpp portal/introspection.cpp portal/settings-portal.cpp
    LAGOM ${PORTAL_LAGOM})

# xdg-desktop-portal-serenity (R5): the backend for org.freedesktop.impl.portal.*
# (contrib/xdg-desktop-portal/). ScreenCast captures with ext-image-copy-capture-v1
# and publishes a PipeWire stream; the main backend also decodes and encodes
# images (wallpaper, screenshots) with LibGfx.
set(protocols ${capture_protocols})
sde_wayland_client_protocol(protocols NAME wlr-virtual-pointer-unstable-v1 XML "${WLR_PROTOCOLS}/wlr-virtual-pointer-unstable-v1.xml")
sde_wayland_client_protocol(protocols NAME virtual-keyboard-unstable-v1 XML "${WLR_PROTOCOLS}/virtual-keyboard-unstable-v1.xml")
sde_wayland_client_protocol(protocols NAME serenity-input-capture XML "${SDE_PROTOCOLS}/serenity-input-capture.xml")
sde_portal_tool(xdg-desktop-portal-serenity
    SOURCES
        portal/dbus.cpp
        portal/introspection.cpp
        portal/host.cpp
        portal/backend.cpp
        portal/screencast.cpp
        portal/remote-desktop.cpp
        portal/input-capture.cpp
        portal/global-shortcuts.cpp
        portal/usb.cpp
        portal/permission-store.cpp
        portal/inhibit.cpp
        ${protocols}
    LAGOM ${PORTAL_LAGOM} gfx compress crypto textcodec unicode url filesystem riff ipc
    LIBRARIES PkgConfig::WAYLAND_CLIENT PkgConfig::PIPEWIRE PkgConfig::XKBCOMMON PkgConfig::GBM PkgConfig::LIBDRM)

# One target for the whole group (scripts/build-tools.sh). The portals and the
# test clients are scripts/build-tools.sh's "SDE tools"; the Lagom-linked portal
# backends come with it.
add_custom_target(sde-tools DEPENDS
    sde-idle sde-dpms sde-screenshot native-app-test wayland-fuzz-test
    ext-workspace-test output-management-test capture-probe screencast-consumer
    virtual-input-test sde-settings-portal xdg-desktop-portal-serenity)
