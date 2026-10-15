#!/usr/bin/env bash
# Install the build dependencies for CI (and for the CI image).
#
# The image is Arch-based: the port builds `-std=c++26` and the suite needs a
# current GCC/CMake, and Arch also matches the packaging target in docs/13.
# Package names here are the Arch ones; `pacman -S --needed` fails loudly on a
# typo, which is the right failure mode the first time this runs.
#
#   scripts/ci-deps.sh            # install
#   scripts/ci-deps.sh --print    # just print the list
set -euo pipefail

REQUIRED=(
    # Build tooling
    base-devel cmake ninja meson pkgconf git python
    # Compositor: wlroots + the SDE's own pkg-config requirements
    wayland wayland-protocols libxkbcommon pixman libdrm libinput libseat
    libdisplay-info libxcb xcb-util-wm
    # Services and portals
    pipewire dbus
    # Xwayland parity tests
    xorg-xwayland
)

OPTIONAL=(
    # A GL stack for the gles2 renderer path (llvmpipe on a runner).
    libglvnd mesa vulkan-icd-loader
    # Direct scanout on hardware (not on a runner, but the build looks for it).
    libliftoff
)

if [[ "${1:-}" == "--print" ]]; then
    printf '%s\n' "${REQUIRED[@]}" "${OPTIONAL[@]}"
    exit 0
fi

command -v pacman >/dev/null || { echo "this script is for the Arch-based CI image" >&2; exit 1; }

pacman -Syu --noconfirm --needed "${REQUIRED[@]}"
# Optional packages: a build must not fail because one of them is unavailable.
pacman -S --noconfirm --needed "${OPTIONAL[@]}" || echo "note: some optional packages were unavailable"
