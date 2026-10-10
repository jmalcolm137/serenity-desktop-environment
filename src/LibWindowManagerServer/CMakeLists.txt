# LibWindowManagerServer: the Serenity WindowManagerServer protocol, served
# in-process for the shell (the Taskbar). SDE-owned.
#
# Window state is sourced from the WindowServer via wlr-foreign-toplevel-
# management, so we generate that client protocol here.
if (NOT DEFINED WLROOTS_PROTOCOL_DIR)
    set(WLROOTS_PROTOCOL_DIR "${SDE_ROOT}/third_party/wlroots/protocol")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)

set(WMS_WAYLAND_DIR "${CMAKE_CURRENT_BINARY_DIR}/wayland")
file(MAKE_DIRECTORY "${WMS_WAYLAND_DIR}")

set(FOREIGN_TOPLEVEL_XML "${WLROOTS_PROTOCOL_DIR}/wlr-foreign-toplevel-management-unstable-v1.xml")
add_custom_command(
    OUTPUT
        "${WMS_WAYLAND_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
        "${WMS_WAYLAND_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" client-header "${FOREIGN_TOPLEVEL_XML}" "${WMS_WAYLAND_DIR}/wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" private-code "${FOREIGN_TOPLEVEL_XML}" "${WMS_WAYLAND_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c"
    DEPENDS "${FOREIGN_TOPLEVEL_XML}"
    VERBATIM
)

add_library(windowmanagerserver SHARED
    WindowManagerServer.cpp
    "${WMS_WAYLAND_DIR}/wlr-foreign-toplevel-management-unstable-v1-protocol.c"
)

target_include_directories(windowmanagerserver PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${WMS_WAYLAND_DIR}"
    ${WAYLAND_CLIENT_INCLUDE_DIRS}
)
target_compile_options(windowmanagerserver PRIVATE -Wno-cast-qual)
target_link_libraries(windowmanagerserver PRIVATE
    LibCore LibIPC LibGfx LibThreading ${WAYLAND_CLIENT_LIBRARIES})
