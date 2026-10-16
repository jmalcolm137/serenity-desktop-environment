#!/usr/bin/env bash
# Build in libgui-wayland's Lagom tree: the framework and the programs that live
# there (Terminal by default, the Taskbar, FileManager, the app suite).
#
#   scripts/build.sh [lagom targets or output paths ...]
#
# The heavy userland (LibGUI/LibGfx/LibCore/LibVT, LibWM, ...) is built by
# libgui-wayland. This script applies the SDE's own patches to the SerenityOS
# checkout, makes sure Lagom is configured with the SDE (SDE_ROOT registers the
# Taskbar and links the SDE system library into Terminal), and asks Lagom's ninja
# for the targets. It does not modify the libgui-wayland repository.
#
# The top-level CMake build calls this as its `lagom` step with the exact files
# the SDE links (lib/liblagom-gui.so, bin/Taskbar, ...), so it stays cheap when
# nothing changed. With SDE_BOOTSTRAP_FRAMEWORK=1, a checkout with no Lagom build
# yet is first built by libgui-wayland's own entry point.
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

# A fresh checkout: build the framework with libgui-wayland's entry point (its
# patches, its configuration) before layering the SDE on top.
if [[ ! -f "$BUILD_DIR/build.ninja" ]]; then
    if [[ "${SDE_BOOTSTRAP_FRAMEWORK:-0}" == 1 ]]; then
        echo "==> No Lagom build in $BUILD_DIR yet: building the framework (libgui-wayland)"
        SERENITY_SRC="$SERENITY_SRC" BUILD_DIR="$BUILD_DIR" JOBS="$JOBS" \
            "$LIBGUI_WAYLAND_ROOT/scripts/build-libgui.sh" --no-tests
    else
        echo "error: no Lagom build in $BUILD_DIR" >&2
        echo "       run $LIBGUI_WAYLAND_ROOT/scripts/build-libgui.sh first" >&2
        exit 1
    fi
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
    if git -C "$SERENITY_SRC" apply -C1 --reverse --check "$patch" >/dev/null 2>&1; then
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
echo "==> patches: $applied_count applied, $already_count already applied, $unverified_count unverifiable"
if [[ -n "$unverified_names" ]]; then
    # A patch the loop cannot verify may simply not be in effect, and the failure it
    # causes appears much later and elsewhere -- as a test failure with no obvious
    # cause, or as a feature that is silently absent. Naming them beats discovering
    # them one build at a time, and stopping beats naming: this branch is how the
    # framework's 0065 (a second copy of 0005-taskbar-window-grouping, overlapping it
    # on five files) silently displaced 0005 instead of colliding with it.
    echo "    unverifiable (they would not be applied):$unverified_names" >&2
    echo "    A patch that neither applies nor reverse-applies no longer matches the tree:" >&2
    echo "    refresh it against the current revision, or find the patch it now overlaps." >&2
    if [[ "${ALLOW_UNVERIFIED_PATCHES:-0}" != 1 ]]; then
        exit 1
    fi
    echo "    ALLOW_UNVERIFIED_PATCHES=1: continuing with them unapplied." >&2
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

# Configure Lagom with the SDE once (a few seconds); afterwards Lagom's own ninja
# re-runs CMake whenever its CMakeLists change.
if ! grep -qxF "SDE_ROOT:UNINITIALIZED=$SDE_ROOT" "$BUILD_DIR/CMakeCache.txt" \
    && ! grep -qxF "SDE_ROOT:PATH=$SDE_ROOT" "$BUILD_DIR/CMakeCache.txt"; then
    echo "==> Configuring the host build with the SDE system root"
    cmake -S "$SERENITY_SRC/Meta/Lagom" -B "$BUILD_DIR" -GNinja -DSDE_ROOT="$SDE_ROOT"
fi

echo "==> Building: ${TARGETS[*]}"
ninja -C "$BUILD_DIR" -j"$JOBS" "${TARGETS[@]}"

echo "==> Done. Binaries in $BUILD_DIR/bin"
