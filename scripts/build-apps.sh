#!/usr/bin/env bash
# Build the SDE's applications.
#
# The unmodified SerenityOS applications that make up the desktop: the
# Applications shipped in the Tasksbar's menus, the games, the demos and the
# development tools. They are the desktop's, not the framework's — libgui-wayland
# builds only LibGUI/LibGfx, the WindowServer shim and the GPU backend. The
# targets are defined in the shared Lagom build by the SDE's patches; this is the
# build entry point, the counterpart of libgui-wayland's scripts/build-libgui.sh.
#
# See docs/24-libgui-wayland-sde-boundary.md.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
JOBS="${JOBS:-$(nproc)}"

APPS=(
    # Applications. SystemMonitor, Debugger and CrashReporter are Linux-native:
    # SystemMonitor reads /proc with a libunwind + libdw thread-stack backend,
    # Debugger drives ptrace directly, and CrashReporter reads a Linux ELF core
    # dump (libgui-wayland patches 0050-0053).
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

if [[ ! -e "$BUILD_DIR/lib/liblagom-gui.so" ]]; then
    echo "error: Serenity userland libraries not found in $BUILD_DIR" >&2
    echo "       run libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

echo "==> Building the SDE applications (${#APPS[@]} targets)"
ninja -C "$BUILD_DIR" -j"$JOBS" "${APPS[@]}" "$@"
