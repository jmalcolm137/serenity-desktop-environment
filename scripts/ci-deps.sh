#!/usr/bin/env bash
# Install the build and test dependencies for CI (and for the CI image).
#
# The image is Arch-based: the port builds `-std=c++26` and the suite needs a
# current GCC/CMake, and Arch also matches the packaging target in docs/13.
# Package names here are the Arch ones; `pacman -S --needed` fails loudly on a
# typo, which is the right failure mode the first time this runs.
#
# The suite was written on EndeavourOS and only ever ran there. On a different
# distribution a *missing tool* does not fail a test: tests guard their external
# clients with `command -v` and print SKIP, so absent packages turn coverage off
# silently. TEST_TOOLS is therefore as deliberate as BUILD — it is the list the
# tests actually reach for.
#
#   scripts/ci-deps.sh            # install
#   scripts/ci-deps.sh --print    # just print the lists
set -euo pipefail

BUILD=(
    # Toolchain
    base-devel cmake ninja meson pkgconf git python
    # Compositor: wlroots + the SDE's own pkg-config requirements
    wayland wayland-protocols libxkbcommon pixman libdrm libinput seatd
    libdisplay-info libxcb xcb-util-wm
    # Services and portals
    pipewire libpipewire dbus
    # Xwayland parity tests
    xorg-xwayland
    # Required by the port's patches, not by the SDE's own CMake: the
    # SystemMonitor patch unwinds target threads with libunwind's ptrace remote
    # access and reads ELF with libdw. On Arch libdw ships in the package named
    # `libelf` (built from elfutils), and the .pc file is libdw.pc.
    libunwind libelf
    # Optional in the SDE's CMake (input capture); cheap, and its absence would
    # only show up as those tests skipping.
    libei
    # Build-time helpers for resource archives.
    zip unzip
)

# External clients and tools the tests invoke. Without them the tests skip.
TEST_TOOLS=(
    xterm xorg-xmessage       # the X11 test clients (WM_CLASS grouping, Xwayland)
    wl-clipboard              # wl-copy/wl-paste: the selection + XWM bridge tests
    imagemagick netpbm        # compare/convert/identify/pnmtopng: screenshot tests
    glib2                     # gdbus: the D-Bus/portal tests
    gdb                       # the debugger test
    desktop-file-utils        # desktop-file-validate: the app-file tests
    systemd                   # coredumpctl: the crash-reporter test
    wireplumber               # the audio graph the audio tests inspect
)

OPTIONAL=(
    # A GL stack for the gles2 renderer path (llvmpipe on a runner).
    libglvnd mesa vulkan-icd-loader
    # Direct scanout on hardware (not on a runner, but the build looks for it).
    libliftoff
)

if [[ "${1:-}" == "--print" ]]; then
    printf '%s\n' "${BUILD[@]}" "${TEST_TOOLS[@]}" "${OPTIONAL[@]}"
    exit 0
fi

command -v pacman >/dev/null || { echo "this script is for the Arch-based CI image" >&2; exit 1; }

pacman -Syu --noconfirm --needed "${BUILD[@]}" "${TEST_TOOLS[@]}"
# Optional packages: a build must not fail because one of them is unavailable.
pacman -S --noconfirm --needed "${OPTIONAL[@]}" || echo "note: some optional packages were unavailable"
