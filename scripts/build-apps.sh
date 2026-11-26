#!/usr/bin/env bash
# Build the SerenityOS application suite (Terminal, FileManager, the demos, ...).
#
# The applications are the desktop's: their Lagom targets come from the SDE's
# patches/0008-sde-lagom-targets.patch (including Calculator, PDFViewer and
# Piano, which moved out of libgui-wayland's apps/). They are built in the Lagom
# tree with the SDE's patches applied, through scripts/build.sh.
#
#   scripts/build-apps.sh            # every application
#   scripts/build-apps.sh --list     # print the target names (used by CMake)
#   scripts/build-apps.sh Terminal   # extra targets, on top of the list
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Applications. Calculator, PDFViewer and Piano used to be built by
# libgui-wayland's apps/ and moved here with the applications; the rest of the
# suite was already the desktop's. Welcome and PartitionEditor have Lagom targets
# (patches/0008) but are not part of the shipped suite.
#
# SystemMonitor, Debugger and CrashReporter are Linux-native:
# SystemMonitor reads /proc with a libunwind + libdw thread-stack backend,
# Debugger drives ptrace directly, and CrashReporter reads a Linux ELF core dump
# (patches 0013, 0023 and 0024, on libgui-wayland's LibCore, LibDebug and
# LibCoredump ports).
APPS=(
    Calculator PDFViewer
    3DFileViewer Tubes About AnalogClock Assistant Browser BrowserSettings
    Calendar CalendarSettings CertificateSettings CharacterMap ClockSettings DisplaySettings Escalator FileManager
    FontEditor GamesSettings Help HexEditor ImageViewer KeyboardMapper
    KeyboardSettings Mail MailSettings Maps MapsSettings MouseSettings NetworkSettings
    Presenter Run Screenshot Settings SpaceAnalyzer Spreadsheet SoundPlayer Piano
    SystemMonitor Debugger CrashReporter TerminalSettings TextEditor ThemeEditor UsersSettings VideoPlayer Weather
    PixelPaint
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
