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

foreach(_lib ak coreminimal corebasic core gfx gui ipc threading url desktop launch imagedecoderclient vt)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()
find_library(LAGOM_main NAMES lagom-main PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

# add_sde_unit_test(<name> SOURCE <file.cpp> [EXTRA_SOURCES ...] [LIBS ...])
#   For logic with no GUI: links AK/LibCore/LibMain only.
function(add_sde_unit_test name)
    cmake_parse_arguments(ARG "" "SOURCE" "LIBS;EXTRA_SOURCES" ${ARGN})
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}" ${ARG_EXTRA_SOURCES})
    target_include_directories(${name} PRIVATE "${SERENITY_SRC}" "${SERENITY_SRC}/Userland/Libraries" "${SERENITY_BUILD_DIR}")
    target_compile_options(${name} PRIVATE -Wno-literal-suffix -fno-exceptions)
    target_link_directories(${name} PRIVATE "${SERENITY_BUILD_DIR}/lib")
    target_link_libraries(${name} PRIVATE
        ${LAGOM_ak}
        ${LAGOM_coreminimal}
        ${LAGOM_corebasic}
        ${LAGOM_core}
        ${ARG_LIBS}
        ${LAGOM_main})
    set_target_properties(${name} PROPERTIES
        BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
        INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
endfunction()

# add_sde_test(<name> SOURCE <file.cpp> LIBS <extra libs>)
function(add_sde_test name)
    cmake_parse_arguments(ARG "" "SOURCE" "LIBS;EXTRA_SOURCES" ${ARGN})
    add_executable(${name} "${CMAKE_CURRENT_SOURCE_DIR}/${ARG_SOURCE}" ${ARG_EXTRA_SOURCES})
    set_property(GLOBAL APPEND PROPERTY SDE_TEST_TARGETS ${name})
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

# Accessibility payload: a frame, Text label, Button/Action, and editable text.
add_sde_test(libwm-accessibility-test SOURCE AccessibilityTest.cpp)

# Changes a setting the desktop owns and reads it back: the shim must forward it
# over serenity-window rather than writing the desktop's configuration.
add_sde_test(libwm-setting-test SOURCE SettingRoundTripTest.cpp)

# A deferred callback with no event loop (the teardown path the coredump showed).
add_sde_test(libwm-eventloop-teardown-test SOURCE EventLoopTeardownTest.cpp)

# "Clear to previous mark" after the scrollback ring has wrapped (a Terminal crash).
add_sde_test(libvt-history-test SOURCE VtHistoryTest.cpp LIBS ${LAGOM_vt})

# One target for the whole group, so scripts/build-tests.sh is a one-liner.
get_property(SDE_TEST_TARGETS GLOBAL PROPERTY SDE_TEST_TARGETS)
add_custom_target(sde-tests DEPENDS ${SDE_TEST_TARGETS})

# The window manager's placement rules (WindowRules.h): pure functions, compiled
# from the compositor's own source, so the test and the compositor cannot drift
# apart. No compositor, no wlroots, no Wayland.
# A pure-logic unit test: AK and the file under test, without LibGUI. Linking
# LibGUI would pull in its static initialisers (the font database wants a
# resource root) for a test that has nothing to do with a GUI.
add_sde_unit_test(libwm-window-rules-test SOURCE WindowRulesTest.cpp
    EXTRA_SOURCES "${CMAKE_SOURCE_DIR}/WindowServer/src/WindowRules.cpp")
target_include_directories(libwm-window-rules-test PRIVATE "${CMAKE_SOURCE_DIR}/WindowServer/src")

# The config validation rules (ConfigRules.h): keybinding combos, output scales,
# booleans and the input ranges. The loader calls exactly these, so the schema in
# docs/14-r4.4-config.md is checked without starting a session.
add_sde_unit_test(libwm-config-rules-test SOURCE ConfigRulesTest.cpp
    EXTRA_SOURCES "${CMAKE_SOURCE_DIR}/WindowServer/src/ConfigRules.cpp"
    LIBS xkbcommon)
target_include_directories(libwm-config-rules-test PRIVATE "${CMAKE_SOURCE_DIR}/WindowServer/src")

# The Taskbar's applet area layout (AppletLayout.h): the configured order and the
# rectangle each applet gets. Pure, so the layout is checked without a Taskbar.
add_sde_unit_test(libwm-applet-layout-test SOURCE AppletLayoutTest.cpp
    EXTRA_SOURCES "${CMAKE_SOURCE_DIR}/WindowServer/src/AppletLayout.cpp")
target_include_directories(libwm-applet-layout-test PRIVATE "${CMAKE_SOURCE_DIR}/WindowServer/src")
