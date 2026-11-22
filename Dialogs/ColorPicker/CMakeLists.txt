cmake_minimum_required(VERSION 3.20)
project(ColorPicker CXX)

# The SDE color picker for the freedesktop Screenshot / PickColor portal
# (docs/15-xdg-desktop-portal-serenity.md).
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../Meta/CMake/SDEGuiApp.cmake")
sde_add_gui_executable(ColorPicker SOURCES src/main.cpp)
