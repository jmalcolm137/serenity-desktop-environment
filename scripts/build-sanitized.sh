#!/usr/bin/env bash
# Build the SDE with ASan/UBSan into its own tree.
#
# The normal build/ tree must stay sanitizer-free: it is what the live session
# runs and what the tests measure. This configures build-<sanitizer> with
# -DSDE_SANITIZE and builds the requested targets (all of them by default).
# The Lagom framework is not rebuilt; it stays uninstrumented, which is enough
# for the sanitizer to catch errors in SDE code and its allocations.
#
#   scripts/build-sanitized.sh                     # everything
#   scripts/build-sanitized.sh WindowServer        # just the compositor
#   SDE_SANITIZE=address,undefined scripts/build-sanitized.sh
#
# Run a sanitized compositor with, for example:
#   COMP=build-address/WindowServer/WindowServer scripts/run-headless-test.sh
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
LAGOM_BUILD_DIR="${SERENITY_BUILD_DIR:-${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
SANITIZE="${SDE_SANITIZE:-address}"
BUILD_DIR="${SDE_SANITIZE_BUILD_DIR:-$SDE_ROOT/build-$SANITIZE}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "$WLROOTS_SRC/install/lib/pkgconfig/wlroots-0.20.pc" ]]; then
    "$SDE_ROOT/scripts/fetch-wlroots.sh"
fi

echo "==> Configuring $BUILD_DIR with -fsanitize=$SANITIZE"
cmake -S "$SDE_ROOT" -B "$BUILD_DIR" -GNinja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSDE_SANITIZE="$SANITIZE" \
    -DSDE_LIBGUI_WAYLAND_ROOT="$LIBGUI_WAYLAND_ROOT" \
    -DSDE_SERENITY_SRC="$SERENITY_SRC" \
    -DSDE_LAGOM_BUILD_DIR="$LAGOM_BUILD_DIR" \
    -DSDE_WLROOTS_SRC="$WLROOTS_SRC" \
    ${SDE_BUILD_APPS:+-DSDE_BUILD_APPS="$SDE_BUILD_APPS"} >/dev/null

if [[ $# -eq 0 ]]; then
    exec cmake --build "$BUILD_DIR" --parallel "$JOBS"
fi
exec cmake --build "$BUILD_DIR" --parallel "$JOBS" --target "$@"
