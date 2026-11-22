cmake_minimum_required(VERSION 3.20)
project(sde-auth-dialog CXX)

# The authentication prompt for sde-polkit-agent. A plain LibGUI app, so the
# dialog is an SDE client like any other and gets the compositor's chrome.
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../Meta/CMake/SDEGuiApp.cmake")
sde_add_gui_executable(sde-auth-dialog SOURCES main.cpp NO_AS_NEEDED)
