cmake_minimum_required(VERSION 3.20)
project(AudioServer CXX)

# The SDE AudioServer service: the SerenityOS AudioServer / AudioManager
# protocols served over the session portals, backed by PipeWire.
#
# It is an *out-of-process* session service: LibWM spawns it on demand from the
# session app directory (SDE_APP_BIN_DIR) when an application connects to
# /tmp/session/<sid>/portal/audio. Applications that speak the AudioServer
# protocol directly (Piano, SoundPlayer, aplay, the Audio applet) reach it there.
# LibAudio itself stays a compositor/desktop-agnostic client library — with its
# PulseAudio backend (docs/24-libgui-wayland-sde-boundary.md). This service must
# not link it: it needs only LibAudio's headers (Resampler.h, Queue.h, Sample.h
# are header-only) and its own PipeWire backend. Linking liblagom-audio would
# pull libpulse/libpulsecommon/libsndfile into a process that is PipeWire-only,
# for ~9 MB resident (docs/25-performance.md §4).
#
# See ../../docs/06-service-layering.md and ../../docs/07-systemserver-roles.md.

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT DEFINED SERENITY_SRC OR NOT DEFINED SERENITY_BUILD_DIR)
    message(FATAL_ERROR "SERENITY_SRC and SERENITY_BUILD_DIR must be set")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

foreach(_lib ak coreminimal corebasic core ipc threading main)
    find_library(LAGOM_${_lib} NAMES lagom-${_lib} PATHS "${SERENITY_BUILD_DIR}/lib" NO_DEFAULT_PATH REQUIRED)
endforeach()

add_executable(AudioServer
    AudioManagerConnection.cpp
    AudioServerConnection.cpp
    ClientAudioStream.cpp
    Mixer.cpp
    PipeWireBackend.cpp
    main.cpp)

target_include_directories(AudioServer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${SERENITY_SRC}"
    "${SERENITY_SRC}/Userland"
    "${SERENITY_SRC}/Userland/Libraries"
    "${SERENITY_SRC}/Userland/Services"
    "${SERENITY_BUILD_DIR}"
    "${SERENITY_BUILD_DIR}/Userland"
    "${SERENITY_BUILD_DIR}/Userland/Libraries"
    "${SERENITY_BUILD_DIR}/Userland/Services")

# PipeWire's C API leaves enum/struct members zero-initialized under designated
# initializers, which the build treats as errors.
target_compile_options(AudioServer PRIVATE -Wno-literal-suffix -fno-exceptions -Wno-missing-field-initializers)
# Keep the static initializers (if any) from being dropped by the linker.
target_link_options(AudioServer PRIVATE "-Wl,--no-as-needed")

target_link_directories(AudioServer PRIVATE "${SERENITY_BUILD_DIR}/lib")
target_link_libraries(AudioServer PRIVATE
    ${LAGOM_ak}
    ${LAGOM_coreminimal}
    ${LAGOM_corebasic}
    ${LAGOM_core}
    ${LAGOM_ipc}
    ${LAGOM_threading}
    ${LAGOM_main}
    PkgConfig::PIPEWIRE)

set_target_properties(AudioServer PROPERTIES
    BUILD_RPATH "${SERENITY_BUILD_DIR}/lib"
    INSTALL_RPATH "${SERENITY_BUILD_DIR}/lib")
