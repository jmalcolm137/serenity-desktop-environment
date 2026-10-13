cmake_minimum_required(VERSION 3.20)
project(WindowServer C CXX)

# The Serenity Desktop Environment's Wayland compositor.
#
# Serenity userland libraries (AK, LibCore, LibGfx, ...) come from the
# libgui-wayland checkout; wlroots is the plumbing. See
# ../scripts/build-window-server.sh.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR)
    message(FATAL_ERROR "SERENITY_SRC and SERENITY_BUILD_DIR must be set (the libgui-wayland checkout and its build directory)")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WLROOTS REQUIRED wlroots-0.20)
pkg_check_modules(WAYLAND_SERVER REQUIRED wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)
pkg_check_modules(DRM REQUIRED libdrm)
# R4.6: input device configuration (accel, tap-to-click, natural scroll, ...).
pkg_check_modules(LIBINPUT REQUIRED libinput)
# pixman: pointer-constraints region tests (zwp-pointer-constraints-v1).
pkg_check_modules(PIXMAN REQUIRED pixman-1)
# R5 InputCapture (docs/17-input-capture.md): the compositor owns the EIS
# implementation via libeis. Optional: a build without libeis simply does not
# offer the serenity_input_capture protocol.
pkg_check_modules(LIBEIS libeis-1.0)

# Serenity userland libraries from the libgui-wayland build.
foreach(_lib ak coreminimal corebasic core gfx unicode ipc compress crypto riff textcodec filesystem url threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

# serenity-window: compositor -> LibWindowServer chrome control (see the XML).
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(SERENITY_WINDOW_PROTOCOL_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocol")
file(MAKE_DIRECTORY "${SERENITY_WINDOW_PROTOCOL_DIR}")
set(SERENITY_WINDOW_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/serenity-window.xml")
add_custom_command(
    OUTPUT
        "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-window-server-protocol.h"
        "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-window-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" server-header "${SERENITY_WINDOW_XML}" "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-window-server-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${SERENITY_WINDOW_XML}" "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-window-protocol.c"
    DEPENDS "${SERENITY_WINDOW_XML}"
    VERBATIM
)

# serenity-input-capture: the private compositor <-> portal-backend protocol
# behind org.freedesktop.impl.portal.InputCapture (docs/17-input-capture.md).
set(SERENITY_INPUT_CAPTURE_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/serenity-input-capture.xml")
add_custom_command(
    OUTPUT
        "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-input-capture-server-protocol.h"
        "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-input-capture-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" server-header "${SERENITY_INPUT_CAPTURE_XML}" "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-input-capture-server-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${SERENITY_INPUT_CAPTURE_XML}" "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-input-capture-protocol.c"
    DEPENDS "${SERENITY_INPUT_CAPTURE_XML}"
    VERBATIM
)

# xdg-session-management-v1: let apps restore their toplevels across a restart
# (R4.2). Standard protocol; wlroots 0.20 has no manager for it, so we generate
# and implement the server side (see src/SessionManager.cpp). The protocol
# references the `xdg_toplevel` interface, and wlroots hides its copy, so we
# generate xdg-shell's server code too (for the interface symbol only).
set(XDG_SESSION_XML "${CMAKE_CURRENT_SOURCE_DIR}/protocol/xdg-session-management-v1.xml")
pkg_get_variable(WAYLAND_PROTOCOLS_DATADIR wayland-protocols pkgdatadir)
set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DATADIR}/stable/xdg-shell/xdg-shell.xml")
add_custom_command(
    OUTPUT
        "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-session-management-v1-server-protocol.h"
        "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-session-management-v1-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" server-header "${XDG_SESSION_XML}" "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-session-management-v1-server-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SESSION_XML}" "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-session-management-v1-protocol.c"
    DEPENDS "${XDG_SESSION_XML}"
    VERBATIM
)
add_custom_command(
    OUTPUT "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-shell-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-shell-protocol.c"
    DEPENDS "${XDG_SHELL_XML}"
    VERBATIM
)

add_executable(WindowServer
    src/main.cpp
    src/AppletManager.cpp
    src/Config.cpp
    src/InputCapture.cpp
    src/Overlays.cpp
    src/SessionManager.cpp
    src/WindowManager.cpp
    src/WindowSwitcher.cpp
    "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-window-protocol.c"
    "${SERENITY_WINDOW_PROTOCOL_DIR}/serenity-input-capture-protocol.c"
    "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-shell-protocol.c"
    "${SERENITY_WINDOW_PROTOCOL_DIR}/xdg-session-management-v1-protocol.c")

target_include_directories(WindowServer PRIVATE
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
    ${LIBINPUT_INCLUDE_DIRS}
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_WINDOW_PROTOCOL_DIR}"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland/Services")

target_compile_options(WindowServer PRIVATE -Wno-literal-suffix -fno-exceptions)
target_compile_definitions(WindowServer PRIVATE WLR_USE_UNSTABLE)

if (DEFINED WLROOTS_PROTOCOL_INCLUDE_DIR)
    target_include_directories(WindowServer PRIVATE "${WLROOTS_PROTOCOL_INCLUDE_DIR}")
endif()

target_link_directories(WindowServer PRIVATE
    ${WLROOTS_LIBRARY_DIRS}
    ${WAYLAND_SERVER_LIBRARY_DIRS}
    ${XKBCOMMON_LIBRARY_DIRS}
    ${DRM_LIBRARY_DIRS}
    ${LIBINPUT_LIBRARY_DIRS}
    "${SERENITY_BUILD_DIR}/lib")

target_link_libraries(WindowServer PRIVATE
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${DRM_LIBRARIES}
    ${LIBINPUT_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_gfx}
    ${LAGOM_unicode}
    ${LAGOM_ipc}
    ${LAGOM_compress}
    ${LAGOM_crypto}
    ${LAGOM_riff}
    ${LAGOM_textcodec}
    ${LAGOM_filesystem}
    ${LAGOM_url}
    ${LAGOM_threading}
    ${LAGOM_main})

# R5 InputCapture: link libeis when present (see src/InputCapture.cpp).
if (LIBEIS_FOUND)
    target_compile_definitions(WindowServer PRIVATE SDE_HAVE_LIBEIS)
    target_include_directories(WindowServer PRIVATE ${LIBEIS_INCLUDE_DIRS})
    target_link_directories(WindowServer PRIVATE ${LIBEIS_LIBRARY_DIRS})
    target_link_libraries(WindowServer PRIVATE ${LIBEIS_LIBRARIES})
endif()

set_target_properties(WindowServer PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
