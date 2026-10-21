cmake_minimum_required(VERSION 3.20)
project(WifiApplet CXX)

# WifiApplet — the SDE Wi-Fi applet for the Taskbar's applet area, modeled on
# KDE Plasma's Networks applet (see ../../docs/19-wifi-applet.md). It is a plain
# SerenityOS LibGUI app; it talks to NetworkManager over the system D-Bus
# (libdbus), so it also links the DBus reference library.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx gui ipc threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

# libgui-wayland's in-process WindowServer shim (LibWM).
find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED dbus-1)

add_executable(WifiApplet
    src/main.cpp
    src/WifiIcon.cpp
    src/NetworkManagerClient.cpp
    src/WifiPopup.cpp
    src/WifiWidget.cpp)

target_include_directories(WifiApplet PRIVATE
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services"
    "${LIBGUI_WAYLAND_SRC}/src"
    ${DBUS_INCLUDE_DIRS})

target_compile_options(WifiApplet PRIVATE -Wno-literal-suffix -fno-exceptions)
target_link_directories(WifiApplet PRIVATE "${SERENITY_BUILD_DIR}/lib" ${DBUS_LIBRARY_DIRS})
target_link_libraries(WifiApplet PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_gfx}
    ${LAGOM_gui}
    ${LAGOM_ipc}
    ${LAGOM_threading}
    ${LAGOM_main}
    ${WINDOWSERVER}
    ${DBUS_LIBRARIES})

set_target_properties(WifiApplet PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
