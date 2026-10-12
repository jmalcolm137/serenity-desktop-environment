# Serenity Desktop Environment system-compatibility library.
#
# Built as part of the libgui-wayland Lagom host build (see
# cmake/SerenityDesktop.cmake), linked into SDE applications. Several SDE apps
# add this subdirectory; guard so the target is created once.
if (NOT TARGET SerenitySystem)
    add_library(SerenitySystem STATIC SerenitySystem.cpp Profiling.cpp)

    target_link_libraries(SerenitySystem PUBLIC LibCore)
    # Profiling.cpp uses perf_event_open's designated-initializer structs.
    target_compile_options(SerenitySystem PRIVATE -Wno-missing-field-initializers)
    find_package(Threads REQUIRED)
    target_link_libraries(SerenitySystem PUBLIC Threads::Threads)
    # Expose this directory's parent so clients can <LibSerenitySystem/...>.
    target_include_directories(SerenitySystem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
endif()
