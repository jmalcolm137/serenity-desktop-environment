#!/usr/bin/env bash
# Build targets in the SDE's single build tree.
#
#   scripts/build-target.sh <target ...>
#
# The top-level CMakeLists.txt owns the build (compiler flags, dependency graph,
# output locations); this script only there-there's the two things CMake needs
# help with: fetching wlroots if it is missing, and configuring the tree the
# first time. Every build-*.sh script is a one-line wrapper around it.
#
# The configure step is idempotent and cheap (~0.5s), so it always runs: a
# CMakeLists.txt edited by hand, or a checkout that has never been configured,
# just works.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SDE_ROOT/build}"
JOBS="${JOBS:-$(nproc)}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"

if [[ ! -f "$WLROOTS_SRC/install/lib/pkgconfig/wlroots-0.20.pc" ]]; then
    "$SDE_ROOT/scripts/fetch-wlroots.sh"
fi

# Wayland protocol headers come from the wlroots checkout; pkg-config from its
# install tree. Both are read by the top-level CMakeLists.txt.
cmake -S "$SDE_ROOT" -B "$BUILD_DIR" -GNinja \
    -DSDE_WLROOTS_SRC="$WLROOTS_SRC" \
    ${SDE_BUILD_APPS:+-DSDE_BUILD_APPS="$SDE_BUILD_APPS"} >/dev/null

if [[ $# -eq 0 ]]; then
    exec cmake --build "$BUILD_DIR" --parallel "$JOBS"
fi
exec cmake --build "$BUILD_DIR" --parallel "$JOBS" --target "$@"
