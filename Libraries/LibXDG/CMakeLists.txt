cmake_minimum_required(VERSION 3.20)
project(LibXDG CXX)

# Shared, zero-GLib implementation of freedesktop.org (XDG) specifications in
# Serenity-style C++ (AK/LibCore). Linked into SDE services, tools, portals and
# tests (docs/37-freedesktop-compliance-plan.md).

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR)
    message(FATAL_ERROR "SERENITY_SRC and SERENITY_BUILD_DIR must be set")
endif()

foreach(_lib ak coreminimal corebasic core)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

add_library(sde-xdg STATIC
    BaseDirs.cpp
    Autostart.cpp)

target_include_directories(sde-xdg PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}")
target_compile_options(sde-xdg PRIVATE -Wno-literal-suffix -fno-exceptions)
target_link_directories(sde-xdg PUBLIC "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(sde-xdg PUBLIC
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core})
