cmake_minimum_required(VERSION 3.20)
project(WebContent CXX)

# The SDE session WebContent service (the LibWeb renderer).
#
# WebView applications (Browser, Mail, Help, ...) reach the renderer through the
# Serenity webcontent portal; LibWM spawns this binary on demand from the session
# app directory (SDE_APP_BIN_DIR). It is a *session* service, so SDE builds it --
# libgui-wayland stays a compositor-agnostic client stack and keeps only its own
# fallback. See ../../docs/06-service-layering.md and ../../docs/07-systemserver-roles.md
# (section 2.3).
#
# Built from the unmodified upstream sources; it links the Serenity userland
# libraries from the libgui-wayland build, plus LibWM so its own outgoing portal
# connects (RequestServer, ImageDecoder) are served the same way.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR OR NOT DEFINED LIBGUI_WAYLAND_SRC)
    message(FATAL_ERROR "SERENITY_SRC, SERENITY_BUILD_DIR and LIBGUI_WAYLAND_SRC must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx unicode ipc threading main filesystem audio imagedecoderclient js web webview locale url)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()
find_library(WINDOWSERVER NAMES windowserver PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)

set(WEBCONTENT_SRC "${SERENITY_SRC}/Userland/Services/WebContent")

add_executable(WebContent
    "${WEBCONTENT_SRC}/ConnectionFromClient.cpp"
    "${WEBCONTENT_SRC}/ConsoleGlobalEnvironmentExtensions.cpp"
    "${WEBCONTENT_SRC}/ImageCodecPluginSerenity.cpp"
    "${WEBCONTENT_SRC}/PageClient.cpp"
    "${WEBCONTENT_SRC}/PageHost.cpp"
    "${WEBCONTENT_SRC}/WebContentConsoleClient.cpp"
    "${WEBCONTENT_SRC}/WebDriverConnection.cpp"
    "${WEBCONTENT_SRC}/main.cpp")

target_include_directories(WebContent PRIVATE
    "${WEBCONTENT_SRC}"
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services"
    "${LIBGUI_WAYLAND_SRC}/src")

target_compile_options(WebContent PRIVATE -Wno-literal-suffix -fno-exceptions)
# LibWM registers its portals from a static initializer; keep the library loaded.
target_link_options(WebContent PRIVATE "-Wl,--no-as-needed")

target_link_directories(WebContent PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(WebContent PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_filesystem}
    ${LAGOM_ipc}
    ${LAGOM_gfx}
    ${LAGOM_audio}
    ${LAGOM_imagedecoderclient}
    ${LAGOM_js}
    ${LAGOM_webview}
    ${LAGOM_web}
    ${LAGOM_locale}
    ${LAGOM_unicode}
    ${LAGOM_url}
    ${LAGOM_threading}
    ${LAGOM_main}
    ${WINDOWSERVER})

set_target_properties(WebContent PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
