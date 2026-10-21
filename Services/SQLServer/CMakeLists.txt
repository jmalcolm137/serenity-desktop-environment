cmake_minimum_required(VERSION 3.20)
project(SQLServer CXX)

# The SDE session SQLServer service.
#
# A multi-client service (IPC::MultiServer): LibWM binds its listening socket and
# hands it over, so it is spawned once from the session app directory
# (SDE_APP_BIN_DIR). Browser needs it for history, bookmarks and cookies. It is a
# *session* service, so SDE builds it. See ../../docs/06-service-layering.md and
# ../../docs/07-systemserver-roles.md (section 2.3).

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR)
    message(FATAL_ERROR "SERENITY_SRC and SERENITY_BUILD_DIR must be set")
endif()

foreach(_lib ak coreminimal corebasic core ipc threading main sql)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

set(SQLSERVER_SRC "${SERENITY_SRC}/Userland/Services/SQLServer")

add_executable(SQLServer
    "${SQLSERVER_SRC}/ConnectionFromClient.cpp"
    "${SQLSERVER_SRC}/DatabaseConnection.cpp"
    "${SQLSERVER_SRC}/SQLStatement.cpp"
    "${SQLSERVER_SRC}/main.cpp")

target_include_directories(SQLServer PRIVATE
    "${SQLSERVER_SRC}"
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services")

target_compile_options(SQLServer PRIVATE -Wno-literal-suffix -fno-exceptions)
target_link_options(SQLServer PRIVATE "-Wl,--no-as-needed")

target_link_directories(SQLServer PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(SQLServer PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_ipc}
    ${LAGOM_sql}
    ${LAGOM_threading}
    ${LAGOM_main})

set_target_properties(SQLServer PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
