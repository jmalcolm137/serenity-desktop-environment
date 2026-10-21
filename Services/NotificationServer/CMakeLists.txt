cmake_minimum_required(VERSION 3.20)
project(NotificationServer CXX)

# The SDE session NotificationServer service.
#
# The unmodified SerenityOS service (spike outcome: reuse; docs/09 section 3).
# LibWM binds its listening socket and hands it over, so it is spawned once from
# the session app directory (SDE_APP_BIN_DIR). It is a *session* service, so SDE
# builds it. See ../../docs/06-service-layering.md.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx gui ipc threading main desktop url)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()
find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

# The GML compiler is a Lagom host tool; the app's own CMakeLists compiles its GML
# through it. Precompile here (the upstream header expects NotificationWidgetGML).
find_program(GMLCOMPILER NAMES GMLCompiler
    PATHS "${SERENITY_BUILD_DIR}/bin" NO_DEFAULT_PATH REQUIRED)

set(NOTIFICATION_SERVER_SRC "${SERENITY_SRC}/Userland/Services/NotificationServer")

set(NOTIFICATION_WIDGET_GML "${CMAKE_CURRENT_BINARY_DIR}/NotificationWidgetGML.cpp")
add_custom_command(
    OUTPUT "${NOTIFICATION_WIDGET_GML}"
    COMMAND sh -c "\"${GMLCOMPILER}\" \"${NOTIFICATION_SERVER_SRC}/NotificationWidget.gml\" > \"${NOTIFICATION_WIDGET_GML}.tmp\""
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${NOTIFICATION_WIDGET_GML}.tmp" "${NOTIFICATION_WIDGET_GML}"
    COMMAND "${CMAKE_COMMAND}" -E remove "${NOTIFICATION_WIDGET_GML}.tmp"
    VERBATIM
    DEPENDS "${GMLCOMPILER}" "${NOTIFICATION_SERVER_SRC}/NotificationWidget.gml"
    MAIN_DEPENDENCY "${NOTIFICATION_SERVER_SRC}/NotificationWidget.gml")

add_executable(NotificationServer
    "${NOTIFICATION_SERVER_SRC}/ConnectionFromClient.cpp"
    "${NOTIFICATION_SERVER_SRC}/NotificationWindow.cpp"
    "${NOTIFICATION_SERVER_SRC}/main.cpp"
    "${NOTIFICATION_WIDGET_GML}")

target_include_directories(NotificationServer PRIVATE
    "${NOTIFICATION_SERVER_SRC}"
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services"
    "${LIBGUI_WAYLAND_SRC}/src")

target_compile_options(NotificationServer PRIVATE -Wno-literal-suffix -fno-exceptions)
target_link_options(NotificationServer PRIVATE "-Wl,--no-as-needed")

target_link_directories(NotificationServer PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(NotificationServer PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_gfx}
    ${LAGOM_gui}
    ${LAGOM_ipc}
    ${LAGOM_desktop}
    ${LAGOM_url}
    ${LAGOM_threading}
    ${LAGOM_main}
    ${WINDOWSERVER})

set_target_properties(NotificationServer PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
