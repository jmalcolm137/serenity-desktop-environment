#!/usr/bin/env bash
# Build the Serenity Desktop Environment against the libgui-wayland host build.
#
# The heavy userland (LibGUI/LibGfx/LibCore/LibVT, LibWM, LibSerenityAudio, ...)
# is built by libgui-wayland. This script applies the SDE's own patches to the
# SerenityOS checkout and links the SDE system library into the already-
# registered Terminal target. It does not modify the libgui-wayland repository.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY_SRC="${SERENITY_SRC:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD_DIR="${BUILD_DIR:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
JOBS="${JOBS:-$(nproc)}"

TARGETS=("$@")
if [[ ${#TARGETS[@]} -eq 0 ]]; then
    TARGETS=(Terminal)
fi

if [[ ! -d "$SERENITY_SRC/.git" ]]; then
    echo "error: SerenityOS checkout not found at $SERENITY_SRC" >&2
    echo "       set LIBGUI_WAYLAND_ROOT/SERENITY_SRC, and run" >&2
    echo "       libgui-wayland/scripts/build-libgui.sh first" >&2
    exit 1
fi

# Apply the SDE's patches to the SerenityOS checkout idempotently.
for patch in "$SDE_ROOT"/patches/*.patch; do
    [[ -e "$patch" ]] || continue
    name="$(basename "$patch")"
    if git -C "$SERENITY_SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "==> $name already applied"
    elif git -C "$SERENITY_SRC" apply --check "$patch" >/dev/null 2>&1; then
        echo "==> Applying $name"
        git -C "$SERENITY_SRC" apply "$patch"
    else
        echo "==> $name already applied (overlapping context)"
    fi
done

echo "==> Configuring the host build with the SDE system root"
cmake -S "$SERENITY_SRC/Meta/Lagom" -B "$BUILD_DIR" -GNinja -DSDE_ROOT="$SDE_ROOT"

echo "==> Building: ${TARGETS[*]}"
ninja -C "$BUILD_DIR" -j"$JOBS" "${TARGETS[@]}"

echo "==> Done. Binaries in $BUILD_DIR/bin"
