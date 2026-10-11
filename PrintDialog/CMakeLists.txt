cmake_minimum_required(VERSION 3.20)
project(PrintDialog CXX)

# The SDE print dialog for the freedesktop Print portal: a plain SerenityOS LibGUI app (see
# ../docs/15-xdg-desktop-portal-serenity.md). It links the Serenity userland
# libraries from the libgui-wayland build.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx gui ipc threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

# libgui-wayland's in-process WindowServer shim (LibWM): LibGUI's
# ConnectionToWindowServer needs it to take over the WindowServer portal.
find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

add_executable(PrintDialog src/main.cpp)

target_include_directories(PrintDialog PRIVATE
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services"
    "${LIBGUI_WAYLAND_SRC}/src")

target_compile_options(PrintDialog PRIVATE -Wno-literal-suffix -fno-exceptions)

target_link_directories(PrintDialog PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(PrintDialog PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_gfx}
    ${LAGOM_gui}
    ${LAGOM_ipc}
    ${LAGOM_threading}
    ${LAGOM_main}
    ${WINDOWSERVER})

set_target_properties(PrintDialog PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
