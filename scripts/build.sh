#!/usr/bin/env bash
# Build the Serenity Desktop Environment against the libgui-wayland host build.
#
# The heavy userland (LibGUI/LibGfx/LibCore/LibVT, LibWM, ...)
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
#
# The third branch does not claim to know whether the patch is in effect. Several
# of these edit files the port's patches also edit, and once an earlier patch has
# been applied `git apply` can neither apply nor reverse this one; no per-patch
# check can separate "already applied" from "written against a different
# revision" in that state. So count them and say so at the end — that single line
# is what makes a later build failure legible. A stale SerenityOS pin once left
# every patch unverifiable, and the build failed much later inside CMake
# ("add_subdirectory ... which is not an existing directory") with no hint why.
applied_count=0
already_count=0
unverified_count=0
unverified_names=""
for patch in "$SDE_ROOT"/patches/*.patch; do
    [[ -e "$patch" ]] || continue
    name="$(basename "$patch")"
    if git -C "$SERENITY_SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "==> $name already applied"
        already_count=$((already_count + 1))
    elif git -C "$SERENITY_SRC" apply -C1 --check "$patch" >/dev/null 2>&1; then
        echo "==> Applying $name"
        git -C "$SERENITY_SRC" apply -C1 "$patch"
        applied_count=$((applied_count + 1))
    else
        echo "==> $name assumed applied (overlapping context; unverifiable)"
        unverified_count=$((unverified_count + 1))
        unverified_names="$unverified_names $name"
    fi
done
echo "==> patches: $applied_count applied, $already_count already applied, $unverified_count assumed"
if [[ -n "$unverified_names" ]]; then
    # Naming them matters: a patch the loop cannot verify may simply not be in
    # effect, and the failure it causes appears much later and elsewhere. Seeing the
    # names in one build beats discovering them one 40-minute build at a time.
    echo "    not verified (they may not be applied at all):$unverified_names"
fi
# Say more only when it is actionable: a checkout that differs from the pin is
# the case where "unassumable" patches mean something.
if [[ "$unverified_count" -gt 0 ]]; then
    # The pin line is SERENITY_REVISION="${SERENITY_REVISION:-<hash>}"; take
    # the first hash-looking token on it.
    pinned="$(grep -m1 '^SERENITY_REVISION=' "${LIBGUI_WAYLAND_ROOT}/scripts/fetch-serenity.sh" 2>/dev/null | grep -oE '[0-9a-f]{7,40}' | head -1)"
    head_rev="$(git -C "$SERENITY_SRC" rev-parse HEAD 2>/dev/null || true)"
    if [[ -n "$pinned" && -n "$head_rev" && "$head_rev" != "$pinned" ]]; then
        echo "    WARNING: this checkout is at ${head_rev:0:12}, not the pinned ${pinned:0:12}." >&2
        echo "             $unverified_count patch(es) could not be verified against it; if a" >&2
        echo "             build step below fails unexpectedly, that is the likely reason." >&2
        echo "             This repo does not pin SerenityOS: the pin lives in" >&2
        echo "             libgui-wayland/scripts/fetch-serenity.sh." >&2
    fi
fi

echo "==> Configuring the host build with the SDE system root"
cmake -S "$SERENITY_SRC/Meta/Lagom" -B "$BUILD_DIR" -GNinja -DSDE_ROOT="$SDE_ROOT"

echo "==> Building: ${TARGETS[*]}"
ninja -C "$BUILD_DIR" -j"$JOBS" "${TARGETS[@]}"

echo "==> Done. Binaries in $BUILD_DIR/bin"
