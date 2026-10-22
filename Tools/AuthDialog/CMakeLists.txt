cmake_minimum_required(VERSION 3.20)
project(sde-auth-dialog CXX)

# The authentication prompt for sde-polkit-agent. A plain LibGUI app, so the
# dialog is an SDE client like any other and gets the compositor's chrome.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx gui ipc threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

add_executable(sde-auth-dialog main.cpp)

target_include_directories(sde-auth-dialog PRIVATE
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services"
    "${LIBGUI_WAYLAND_SRC}/src")

target_compile_options(sde-auth-dialog PRIVATE -Wno-literal-suffix -fno-exceptions)

target_link_directories(sde-auth-dialog PRIVATE "${SERENITY_BUILD_DIR}/lib")
# Arch passes --as-needed; this app does not reference a windowserver symbol
# directly, but it must link the in-process shim or GUI::Application::create()
# cannot connect to the WindowServer portal.
target_link_options(sde-auth-dialog PRIVATE "LINKER:--no-as-needed")
target_link_libraries(sde-auth-dialog PRIVATE
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

set_target_properties(sde-auth-dialog PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
