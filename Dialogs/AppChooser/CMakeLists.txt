cmake_minimum_required(VERSION 3.20)
project(AppChooserDialog CXX)

# The SDE application chooser for the freedesktop OpenURI / AppChooser portal
# (docs/15-xdg-desktop-portal-serenity.md).
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../Meta/CMake/SDEGuiApp.cmake")
sde_add_gui_executable(AppChooserDialog SOURCES src/main.cpp)
