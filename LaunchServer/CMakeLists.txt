cmake_minimum_required(VERSION 3.20)
project(LaunchServer CXX)

# The SDE session LaunchServer (see ../docs/06-service-layering.md). It reuses
# libgui-wayland's LaunchServerConnection and Launch::HandlerDatabase, so it links
# the Serenity userland libraries from the libgui-wayland build.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
endif()

foreach(_lib ak coreminimal corebasic core ipc url threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

# The shared launch-handler resolver and the (header-only) LaunchServerConnection
# live in libgui-wayland; including the latter directly keeps this service free
# of LibGUI (and of the WindowServer shim).
find_library(LAGOM_launch NAMES lagom-launch PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

add_executable(LaunchServer src/main.cpp)

target_include_directories(LaunchServer PRIVATE
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland/Services"
    "${LIBGUI_WAYLAND_SRC}/src")

target_compile_options(LaunchServer PRIVATE -Wno-literal-suffix -fno-exceptions)

target_link_directories(LaunchServer PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(LaunchServer PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_ipc}
    ${LAGOM_url}
    ${LAGOM_threading}
    ${LAGOM_main}
    ${LAGOM_launch})

set_target_properties(LaunchServer PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
