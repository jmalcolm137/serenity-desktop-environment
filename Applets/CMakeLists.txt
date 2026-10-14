cmake_minimum_required(VERSION 3.20)
project(SDEApplets CXX)

# The SerenityOS Taskbar applets, built by SDE (not by libgui-wayland): they are
# part of the desktop environment. They are compiled from the unmodified upstream
# sources in the SerenityOS checkout and link the Serenity userland libraries plus
# libgui-wayland's in-process WindowServer shim (LibWM), exactly like BatteryApplet.
# See docs/17-... (applets) and docs/06-service-layering.md.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC OR NOT DEFINED SDE_ROOT)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR, LIBGUI_WAYLAND_SRC and SDE_ROOT must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx gui ipc threading main config keyboard audio)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()
find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

set(APPLETS_SRC "${SERENITY_SRC}/Userland/Applets")

# add_sde_applet(<Name> SOURCES a.cpp b.cpp [LIBS lib1 lib2])
#
# The unmodified upstream sources are used by default. An applet that needs a
# Linux replacement for a SerenityOS kernel interface (Network's
# /sys/kernel/net/adapters, ResourceGraph's /sys/kernel/*) has a local copy under
# Applets/<Name>/; those files take precedence, file by file.
function(add_sde_applet name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    set(_local_dir "${CMAKE_CURRENT_SOURCE_DIR}/${name}")
    set(_sources "")
    foreach(_s ${ARG_SOURCES})
        if (EXISTS "${_local_dir}/${_s}")
            list(APPEND _sources "${_local_dir}/${_s}")
        else()
            list(APPEND _sources "${APPLETS_SRC}/${name}/${_s}")
        endif()
    endforeach()

    add_executable(${name} ${_sources})
    if (EXISTS "${_local_dir}")
        target_include_directories(${name} BEFORE PRIVATE "${_local_dir}")
    endif()
    target_include_directories(${name} PRIVATE
        "${APPLETS_SRC}/${name}"
        "${SERENITY_SRC}"
        "${SERENITY_SRC}/Userland"
        "${SERENITY_SRC}/Userland/Libraries"
        "${SERENITY_SRC}/Userland/Services"
        "${SERENITY_BUILD_DIR}"
        "${SERENITY_BUILD_DIR}/Userland"
        "${SERENITY_BUILD_DIR}/Userland/Libraries"
        "${SERENITY_BUILD_DIR}/Userland/Services"
        "${LIBGUI_WAYLAND_SRC}/src")
    # Host stubs for Serenity-only headers (WindowServer/Window.h -> WMEventMask).
    target_include_directories(${name} BEFORE PRIVATE "${SDE_ROOT}/compat")
    target_compile_options(${name} PRIVATE -Wno-literal-suffix -fno-exceptions)
    target_link_options(${name} PRIVATE "-Wl,--no-as-needed")
    target_link_directories(${name} PRIVATE "${SERENITY_BUILD_DIR}/lib")
    target_link_libraries(${name} PRIVATE
        ${LAGOM_ak} ${LAGOM_coreminimal} ${LAGOM_corebasic} ${LAGOM_core}
        ${LAGOM_gfx} ${LAGOM_gui} ${LAGOM_ipc} ${LAGOM_threading} ${LAGOM_main}
        ${WINDOWSERVER}
        ${ARG_LIBS})
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
        INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
endfunction()

# Unmodified upstream sources; only the build wiring lives here.
add_sde_applet(WorkspacePicker SOURCES DesktopStatusWindow.cpp main.cpp)
add_sde_applet(Network SOURCES main.cpp)
add_sde_applet(ResourceGraph SOURCES main.cpp)
add_sde_applet(Keymap SOURCES KeymapStatusWindow.cpp KeymapStatusWidget.cpp main.cpp LIBS ${LAGOM_keyboard})
add_sde_applet(Audio SOURCES main.cpp LIBS ${LAGOM_audio} ${LAGOM_config})
add_sde_applet(ClipboardHistory SOURCES ClipboardHistoryModel.cpp main.cpp LIBS ${LAGOM_config})
