# Serenity Desktop Environment system-compatibility library.
#
# Built as part of the libgui-wayland Lagom host build (see
# cmake/SerenityDesktop.cmake), linked into SDE applications.
add_library(SerenitySystem STATIC SerenitySystem.cpp)

target_link_libraries(SerenitySystem PUBLIC LibCore)
# Expose this directory's parent so clients can <LibSerenitySystem/SerenitySystem.h>.
target_include_directories(SerenitySystem PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
