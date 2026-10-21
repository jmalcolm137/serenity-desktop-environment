cmake_minimum_required(VERSION 3.20)
project(RequestServer CXX)

# The SDE session RequestServer service (the HTTP/TLS backend for page loads).
#
# WebContent and the WebView applications reach it through the Serenity request
# portal; LibWM spawns this binary on demand from the session app directory
# (SDE_APP_BIN_DIR). It is a *session* service, so SDE builds it. See
# ../../docs/06-service-layering.md and ../../docs/07-systemserver-roles.md (section 2.3).

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR)
    message(FATAL_ERROR "SERENITY_SRC and SERENITY_BUILD_DIR must be set")
endif()

foreach(_lib ak coreminimal corebasic core ipc threading main crypto gemini http tls websocket url)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

set(REQUESTSERVER_SRC "${SERENITY_SRC}/Userland/Services/RequestServer")

add_executable(RequestServer
    "${REQUESTSERVER_SRC}/ConnectionFromClient.cpp"
    "${REQUESTSERVER_SRC}/ConnectionCache.cpp"
    "${REQUESTSERVER_SRC}/GeminiProtocol.cpp"
    "${REQUESTSERVER_SRC}/GeminiRequest.cpp"
    "${REQUESTSERVER_SRC}/HttpProtocol.cpp"
    "${REQUESTSERVER_SRC}/HttpRequest.cpp"
    "${REQUESTSERVER_SRC}/HttpsProtocol.cpp"
    "${REQUESTSERVER_SRC}/HttpsRequest.cpp"
    "${REQUESTSERVER_SRC}/Protocol.cpp"
    "${REQUESTSERVER_SRC}/Request.cpp"
    "${REQUESTSERVER_SRC}/main.cpp")

target_include_directories(RequestServer PRIVATE
    "${REQUESTSERVER_SRC}"
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services")

target_compile_options(RequestServer PRIVATE -Wno-literal-suffix -fno-exceptions)
target_link_options(RequestServer PRIVATE "-Wl,--no-as-needed")

target_link_directories(RequestServer PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(RequestServer PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_crypto}
    ${LAGOM_ipc}
    ${LAGOM_gemini}
    ${LAGOM_http}
    ${LAGOM_tls}
    ${LAGOM_websocket}
    ${LAGOM_url}
    ${LAGOM_threading}
    ${LAGOM_main})

set_target_properties(RequestServer PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
