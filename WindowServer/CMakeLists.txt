cmake_minimum_required(VERSION 3.20)
project(WindowServer CXX)

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

# Serenity userland libraries from the libgui-wayland build.
foreach(_lib ak coreminimal corebasic core gfx unicode ipc compress crypto riff textcodec filesystem url threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

add_executable(WindowServer src/main.cpp)

target_include_directories(WindowServer PRIVATE
    ${WLROOTS_INCLUDE_DIRS}
    ${WAYLAND_SERVER_INCLUDE_DIRS}
    ${XKBCOMMON_INCLUDE_DIRS}
    ${DRM_INCLUDE_DIRS}
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
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
    "${SERENITY_BUILD_DIR}/lib")

target_link_libraries(WindowServer PRIVATE
    ${WLROOTS_LIBRARIES}
    ${WAYLAND_SERVER_LIBRARIES}
    ${XKBCOMMON_LIBRARIES}
    ${DRM_LIBRARIES}
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

set_target_properties(WindowServer PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
