cmake_minimum_required(VERSION 3.20)
project(SDE-Tests CXX)

# The SDE's test clients.
#
# LibGUI / LibWindowServer programs that exercise *this desktop's* integration:
# the notify and tooltip portals, the session app directory (the `/bin` shadow),
# the font-parity decision, the session-service layering (deferral vs in-process
# fallback), and the Profiler / SystemMonitor host backends. Their drivers and
# assertions are this repo's `scripts/run-*-test.sh`, and the payloads encode SDE
# expectations, so they are the SDE's, not the framework's — see
# docs/24-libgui-wayland-sde-boundary.md.
#
# They build against the libgui-wayland Lagom build: the Serenity userland
# libraries and the LibWindowServer (LibWM) shim.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx gui ipc threading url desktop launch imagedecoderclient)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()
find_library(LAGOM_main NAMES lagom-main PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

# add_sde_test(<name> SOURCE <file.cpp> LIBS <extra libs>)
function(add_sde_test name)
    cmake_parse_arguments(ARG "" "SOURCE" "LIBS" ${ARGN})
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}")
    target_include_directories(${name} PRIVATE
        "${SERENITY_SRC}"
        "${SERENITY_SRC}/Userland"
        "${SERENITY_SRC}/Userland/Libraries"
        "${SERENITY_SRC}/Userland/Services"
        "${SERENITY_BUILD_DIR}"
        "${SERENITY_BUILD_DIR}/Userland"
        "${SERENITY_BUILD_DIR}/Userland/Libraries"
        "${SERENITY_BUILD_DIR}/Userland/Services"
        "${LIBGUI_WAYLAND_SRC}/src")
    target_compile_options(${name} PRIVATE -Wno-literal-suffix -fno-exceptions)
    # LibWM registers its portal from a static initializer; keep it loaded.
    target_link_options(${name} PRIVATE "-Wl,--no-as-needed")
    target_link_directories(${name} PRIVATE "${SERENITY_BUILD_DIR}/lib")
    target_link_libraries(${name} PRIVATE
        ${LAGOM_ak}
        ${LAGOM_coreminimal}
        ${LAGOM_corebasic}
        ${LAGOM_core}
        ${LAGOM_gfx}
        ${LAGOM_gui}
        ${LAGOM_ipc}
        ${LAGOM_threading}
        ${WINDOWSERVER}
        ${ARG_LIBS}
        ${LAGOM_main})
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
        INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
endfunction()

# Starts a LibGUI DragOperation on a mouse press, to exercise the DnD bridge.
add_sde_test(libwm-drag-test SOURCE DragTest.cpp)

# Probes the launch and image session-service portals, with and without an
# external session service (in-process fallback vs. deferral).
add_sde_test(libwm-services-test SOURCE ServiceLayeringTest.cpp
    LIBS ${LAGOM_url} ${LAGOM_desktop} ${LAGOM_launch} ${LAGOM_imagedecoderclient})

# Shows a notification through LibGUI::Notification, to exercise the notify portal.
add_sde_test(libwm-notify-test SOURCE NotificationTest.cpp LIBS ${LAGOM_url})

# Shows a tooltip, to exercise the tooltip layer surface.
add_sde_test(libwm-tooltip-test SOURCE TooltipTest.cpp)

# Confirms the font-parity decision (bitmap Katica, no size-based zoom).
add_sde_test(libwm-font-parity-test SOURCE FontParityTest.cpp)

# Reads /proc for SystemMonitor's host backend.
add_sde_test(libwm-procstat-test SOURCE ProcStatTest.cpp)

# Resolves executables through the session's /bin shadow and app directory.
add_sde_test(libwm-binpath-test SOURCE BinPathTest.cpp)

# Launches an executable file directly (the desktop's "Display Settings" entry).
add_sde_test(libwm-launch-exec-test SOURCE LaunchExecTest.cpp
    LIBS ${LAGOM_url} ${LAGOM_desktop} ${LAGOM_imagedecoderclient})

# Opens a ComboBox popup, to exercise the popup positioning.
add_sde_test(libwm-combo-test SOURCE ComboBoxTest.cpp)

# Changes a setting the desktop owns and reads it back: the shim must forward it
# over serenity-window rather than writing the desktop's configuration.
add_sde_test(libwm-setting-test SOURCE SettingRoundTripTest.cpp)

# A deferred callback with no event loop (the teardown path the coredump showed).
add_sde_test(libwm-eventloop-teardown-test SOURCE EventLoopTeardownTest.cpp)
