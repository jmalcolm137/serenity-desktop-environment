#!/usr/bin/env bash
# Build the SerenityOS application suite (Terminal, FileManager, the demos, ...).
#
# The applications live in the Lagom tree, not in an SDE component, so this is a
# wrapper around scripts/build.sh, which builds them there with the SDE's patches
# applied.
#
#   scripts/build-apps.sh            # every application
#   scripts/build-apps.sh --list     # print the target names (used by CMake)
#   scripts/build-apps.sh Terminal   # extra targets, on top of the list
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Applications. SystemMonitor, Debugger and CrashReporter are Linux-native:
# SystemMonitor reads /proc with a libunwind + libdw thread-stack backend,
# Debugger drives ptrace directly, and CrashReporter reads a Linux ELF core dump
# (libgui-wayland patches 0050-0053).
APPS=(
    Calculator PDFViewer
    3DFileViewer Tubes About AnalogClock Assistant Browser BrowserSettings
    Calendar CalendarSettings CertificateSettings CharacterMap ClockSettings DisplaySettings Escalator FileManager
    FontEditor GamesSettings Help HexEditor ImageViewer KeyboardMapper
    KeyboardSettings Mail MailSettings Maps MapsSettings MouseSettings NetworkSettings
    Presenter Run Screenshot Settings SpaceAnalyzer Spreadsheet SoundPlayer Piano
    SystemMonitor Debugger CrashReporter TerminalSettings TextEditor ThemeEditor UsersSettings VideoPlayer Weather
    # The games and DevTools (the Taskbar's Games and Development menus).
    2048 BrickGame Chess ColorLines FlappyBug Flood GameOfLife Hearts MasterWord
    Minesweeper Snake Solitaire Spider ChessEngine Sudoku
    GMLPlayground HackStudio Profiler SQLStudio
    # The demos and Magnifier (the Taskbar's Demos and Utilities menus).
    CatDog Eyes Gradient LibGfxDemo LibGfxScaleDemo Mandelbrot ModelGallery
    Screensaver Starfield WidgetGallery Magnifier
)

if [[ "${1:-}" == "--list" ]]; then
    echo "${APPS[@]}"
    exit 0
fi

echo "==> Building the SDE applications (${#APPS[@]} targets)"
exec "$SDE_ROOT/scripts/build.sh" "${APPS[@]}" "$@"
