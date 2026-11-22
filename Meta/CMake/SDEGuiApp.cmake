# Shared helper for SDE executables built against SerenityOS's Lagom LibGUI
# stack and libgui-wayland's in-process WindowServer shim (LibWM).
#
# Usage:
#   sde_add_gui_executable(<name>
#       SOURCES <file1> [<file2> ...]
#       [EXTRA_LIBS <lib1> ...]
#       [NO_AS_NEEDED])
#
# Works both from the top-level CMakeLists.txt (via Meta/CMake/SDELagom.cmake)
# and when a component is configured standalone with SERENITY_SRC,
# SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC.

include_guard(GLOBAL)

function(sde_add_gui_executable target_name)
    cmake_parse_arguments(ARG "NO_AS_NEEDED" "" "SOURCES;EXTRA_LIBS" ${ARGN})

    if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
        message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
    endif()

    foreach(_lib ak coreminimal corebasic core gfx gui ipc threading main)
        find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
    endforeach()
    find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

    add_executable(${target_name} ${ARG_SOURCES})
    target_include_directories(${target_name} PRIVATE
        "${SERENITY_SRC}"
        "${SERENITY_SRC}/Userland"
        "${SERENITY_SRC}/Userland/Libraries"
        "${SERENITY_SRC}/Userland/Services"
        "${SERENITY_BUILD_DIR}"
        "${SERENITY_BUILD_DIR}/Userland"
        "${SERENITY_BUILD_DIR}/Userland/Libraries"
        "${SERENITY_BUILD_DIR}/Userland/Services"
        "${LIBGUI_WAYLAND_SRC}/src")
    target_compile_options(${target_name} PRIVATE -Wno-literal-suffix -fno-exceptions)
    target_link_directories(${target_name} PRIVATE "${SERENITY_BUILD_DIR}/lib")
    if (ARG_NO_AS_NEEDED)
        # Keep the in-process WindowServer shim loaded even when the app does not
        # reference a shim symbol directly (Arch passes --as-needed by default).
        target_link_options(${target_name} PRIVATE "LINKER:--no-as-needed")
    endif()
    target_link_libraries(${target_name} PRIVATE
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
        ${ARG_EXTRA_LIBS})
    set_target_properties(${target_name} PROPERTIES
        BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
        INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
endfunction()
