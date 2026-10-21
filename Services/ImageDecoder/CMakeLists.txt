cmake_minimum_required(VERSION 3.20)
project(ImageDecoder CXX)

# The SDE session ImageDecoder service (see ../../docs/06-service-layering.md). It
# reuses SerenityOS's own ImageDecoder::ConnectionFromClient (compiled from the
# Serenity tree) so decoding behaves identically to the in-process fallback in
# libgui-wayland. It links the Serenity userland libraries from the
# libgui-wayland build, and must not link LibGUI.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR)
    message(FATAL_ERROR "SERENITY_SRC and SERENITY_BUILD_DIR must be set")
endif()

foreach(_lib ak coreminimal corebasic core gfx unicode ipc threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

add_executable(ImageDecoder
    src/main.cpp
    "${SERENITY_SRC}/Userland/Services/ImageDecoder/ConnectionFromClient.cpp")

target_include_directories(ImageDecoder PRIVATE
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    # Some LibGfx headers (e.g. TIFFMetadata.h) are generated into the build tree.
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services")

target_compile_options(ImageDecoder PRIVATE -Wno-literal-suffix -fno-exceptions)

target_link_directories(ImageDecoder PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(ImageDecoder PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_gfx}
    ${LAGOM_unicode}
    ${LAGOM_ipc}
    ${LAGOM_threading}
    ${LAGOM_main})

set_target_properties(ImageDecoder PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
