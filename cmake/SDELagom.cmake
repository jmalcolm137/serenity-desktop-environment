# The SerenityOS framework the SDE builds on: libgui-wayland's Lagom build
# (AK, LibCore, LibGfx, LibGUI, LibWeb, the windowserver shim, ...) and the
# applications that are built inside it (the Taskbar, Terminal, the app suite).
#
# Lagom is its own CMake project with its own entry point
# (libgui-wayland/scripts/build-libgui.sh), so it is not added as a subdirectory.
# Instead the SDE build drives it: the `lagom` target runs scripts/build.sh, which
# applies the SDE's patches to the shared SerenityOS checkout and asks Lagom's
# ninja for exactly the files the SDE uses. Those files are the target's
# byproducts, so a component that links one of them depends on it: change LibGUI
# and `cmake --build build` rebuilds it in Lagom and relinks what uses it. A
# no-op pass through Lagom costs well under a second.
#
# Inputs (cache variables):
#   SDE_LIBGUI_WAYLAND_ROOT  the libgui-wayland checkout (default: a sibling)
#   SDE_SERENITY_SRC         its SerenityOS checkout
#   SDE_LAGOM_BUILD_DIR      its Lagom build directory
#   SDE_BUILD_APPS           also build the application suite in Lagom

set(SDE_LIBGUI_WAYLAND_ROOT "${CMAKE_SOURCE_DIR}/../libgui-wayland" CACHE PATH "The libgui-wayland checkout")
get_filename_component(SDE_LIBGUI_WAYLAND_ROOT "${SDE_LIBGUI_WAYLAND_ROOT}" ABSOLUTE)
set(SDE_SERENITY_SRC "${SDE_LIBGUI_WAYLAND_ROOT}/serenity" CACHE PATH "libgui-wayland's SerenityOS checkout")
set(SDE_LAGOM_BUILD_DIR "${SDE_LIBGUI_WAYLAND_ROOT}/build/lagom" CACHE PATH "libgui-wayland's Lagom build directory")
option(SDE_BUILD_APPS "Also build the SerenityOS application suite (in Lagom)" OFF)

if (NOT EXISTS "${SDE_SERENITY_SRC}/.git")
    message(FATAL_ERROR "No SerenityOS checkout at ${SDE_SERENITY_SRC}.\n"
        "Fetch it with ${SDE_LIBGUI_WAYLAND_ROOT}/scripts/fetch-serenity.sh, or point "
        "SDE_LIBGUI_WAYLAND_ROOT / SDE_SERENITY_SRC at it.")
endif()

# The names the components use (they predate this file and still build on their
# own when given these).
set(SERENITY_SRC "${SDE_SERENITY_SRC}")
set(SERENITY_BUILD_DIR "${SDE_LAGOM_BUILD_DIR}")
set(LIBGUI_WAYLAND_SRC "${SDE_LIBGUI_WAYLAND_ROOT}")

# Every Lagom library a component links. The components look each one up with
# find_library(LAGOM_<name> ...); setting the cache entry here means that lookup
# succeeds before Lagom has been built (a fresh checkout), and every component
# links the same file, which the `lagom` target below produces.
set(SDE_LAGOM_LIBRARIES
    ak audio compress config core corebasic coreminimal crypto desktop filesystem
    gemini gfx gui http imagedecoderclient ipc js keyboard launch locale main riff
    sql textcodec threading tls unicode url vt web websocket webview)
set(SDE_LAGOM_FILES)
foreach(_lib IN LISTS SDE_LAGOM_LIBRARIES)
    if (_lib STREQUAL "main")
        # LibMain is the one static library.
        set(_file "${SDE_LAGOM_BUILD_DIR}/lib/liblagom-main.a")
    else()
        set(_file "${SDE_LAGOM_BUILD_DIR}/lib/liblagom-${_lib}.so")
    endif()
    set(LAGOM_${_lib} "${_file}" CACHE FILEPATH "Lagom's lib${_lib}" FORCE)
    list(APPEND SDE_LAGOM_FILES "${_file}")
endforeach()
set(WINDOWSERVER "${SDE_LAGOM_BUILD_DIR}/lib/libwindowserver.so" CACHE FILEPATH "libgui-wayland's windowserver shim" FORCE)
list(APPEND SDE_LAGOM_FILES "${WINDOWSERVER}")

# The applications built inside Lagom with the SDE's patches (scripts/build.sh
# builds Terminal by default; the session starts the Taskbar and FileManager).
set(SDE_LAGOM_PROGRAMS Taskbar Terminal FileManager)
if (SDE_BUILD_APPS)
    # The suite scripts/build-apps.sh names; kept in one place there.
    execute_process(
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/build-apps.sh" --list
        OUTPUT_VARIABLE _apps OUTPUT_STRIP_TRAILING_WHITESPACE
        COMMAND_ERROR_IS_FATAL ANY)
    separate_arguments(_apps UNIX_COMMAND "${_apps}")
    list(APPEND SDE_LAGOM_PROGRAMS ${_apps})
    list(REMOVE_DUPLICATES SDE_LAGOM_PROGRAMS)
endif()
set(SDE_LAGOM_PROGRAM_FILES)
foreach(_program IN LISTS SDE_LAGOM_PROGRAMS)
    list(APPEND SDE_LAGOM_PROGRAM_FILES "${SDE_LAGOM_BUILD_DIR}/bin/${_program}")
endforeach()

# A fresh framework (no Lagom build yet) is bootstrapped by libgui-wayland's own
# entry point; after that scripts/build.sh only asks Lagom's ninja for files.
set(_lagom_paths)
foreach(_file IN LISTS SDE_LAGOM_FILES SDE_LAGOM_PROGRAM_FILES)
    file(RELATIVE_PATH _relative "${SDE_LAGOM_BUILD_DIR}" "${_file}")
    list(APPEND _lagom_paths "${_relative}")
endforeach()
add_custom_target(lagom
    COMMAND "${CMAKE_COMMAND}" -E env
        "LIBGUI_WAYLAND_ROOT=${SDE_LIBGUI_WAYLAND_ROOT}"
        "SERENITY_SRC=${SDE_SERENITY_SRC}"
        "BUILD_DIR=${SDE_LAGOM_BUILD_DIR}"
        "SDE_BOOTSTRAP_FRAMEWORK=1"
        "${CMAKE_SOURCE_DIR}/scripts/build.sh" ${_lagom_paths}
    BYPRODUCTS ${SDE_LAGOM_FILES} ${SDE_LAGOM_PROGRAM_FILES}
    COMMENT "Building the framework and the SDE's Lagom programs (libgui-wayland)"
    USES_TERMINAL
    VERBATIM)
