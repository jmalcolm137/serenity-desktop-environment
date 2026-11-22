cmake_minimum_required(VERSION 3.20)
project(ScreenshotOverlay CXX)

# The SDE interactive region-selection overlay for the freedesktop Screenshot
# portal (docs/15-xdg-desktop-portal-serenity.md).
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../Meta/CMake/SDEGuiApp.cmake")
sde_add_gui_executable(ScreenshotOverlay SOURCES src/main.cpp)
