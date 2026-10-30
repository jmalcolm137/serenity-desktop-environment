#!/usr/bin/env bash
# ci-tier: full
# Headless test for the split framework package: stage a framework prefix from a
# built libgui-wayland tree, then build and run the SDE compositor against it with
# SDE_PREBUILT_FRAMEWORK=ON.
#
# This is the packaging seam that has to be right before a split PKGBUILD can be
# trusted: the SDE needs the framework's libs/apps AND its generated headers and
# source, but must not invoke the libgui-wayland build itself.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/test-env.sh"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
WLROOTS_SRC="${WLROOTS_SRC:-$SDE_ROOT/third_party/wlroots}"
RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-split.XXXXXX")"
chmod 700 "$RUNTIME"
trap 'rm -rf "$RUNTIME"' EXIT

[[ -x "$BUILD/bin/GMLCompiler" && -d "$BUILD/lib" ]] || {
    echo "SKIP: the framework is not built at $BUILD"
    exit 0
}

echo "=== staging the framework prefix ==="
"$SDE_ROOT/scripts/install-framework.sh" --destdir "$RUNTIME/root" --prefix /usr/lib/sde \
    > "$RUNTIME/framework-install.log" 2>&1 || {
    echo "  FAIL: install-framework"
    tail -30 "$RUNTIME/framework-install.log"
    exit 1
}
PREFIX="$RUNTIME/root/usr/lib/sde"

fail=0
check() {
    if eval "$2"; then
        echo "  ok: $1"
    else
        echo "  FAIL: $1"
        fail=1
    fi
}
check "runtime libraries were installed" "[[ -n \"\$(find '$PREFIX/lib' -name 'liblagom-gui.so*' -print -quit)\" ]]"
check "applications were installed" "[[ -x '$PREFIX/bin/Taskbar' && -x '$PREFIX/bin/DisplaySettings' ]]"
check "resources were installed" "[[ -d '$PREFIX/share/res/themes' ]]"
check "Serenity manual pages were installed" "[[ -f '$RUNTIME/root/usr/share/man/man1/Applications/Terminal.md' && -f '$RUNTIME/root/usr/share/man/man6/Hearts.md' ]]"
check "generated headers were installed" "[[ -f '$PREFIX/framework/AK/Debug.h' ]]"
check "the Serenity source was installed" "[[ -f '$PREFIX/framework/serenity/AK/Debug.h.in' ]]"
check "the LibWM headers were installed" "[[ -f '$PREFIX/framework/src/LibWM.cpp' ]]"
check "the GML compiler was installed" "[[ -x '$PREFIX/framework/bin/GMLCompiler' ]]"

echo "=== building WindowServer against the staged framework ==="
cmake -S "$SDE_ROOT" -B "$RUNTIME/build" -GNinja \
    -DSDE_LIBGUI_WAYLAND_ROOT="$PREFIX/framework" \
    -DSDE_SERENITY_SRC="$PREFIX/framework/serenity" \
    -DSDE_LAGOM_BUILD_DIR="$PREFIX" \
    -DSDE_SERENITY_BUILD_DIR="$PREFIX/framework" \
    -DSDE_PREBUILT_FRAMEWORK=ON \
    -DSDE_WLROOTS_SRC="$WLROOTS_SRC" \
    -DSDE_BUILD_APPS=OFF > "$RUNTIME/configure.log" 2>&1 || {
    echo "  FAIL: configure"
    tail -30 "$RUNTIME/configure.log"
    exit 1
}
cmake --build "$RUNTIME/build" --parallel "$(nproc)" --target WindowServer \
    > "$RUNTIME/build.log" 2>&1 || {
    echo "  FAIL: build"
    tail -30 "$RUNTIME/build.log"
    exit 1
}
check "the prebuilt-framework WindowServer links" "[[ -x '$RUNTIME/build/WindowServer/WindowServer' ]]"
if grep -q 'Building the framework' "$RUNTIME/build.log"; then
    echo "  FAIL: the prebuilt build tried to drive libgui-wayland"
    fail=1
else
    echo "  ok: the prebuilt build did not invoke libgui-wayland"
fi

echo "=== running the R1 smoke test from the prebuilt build ==="
COMP="$RUNTIME/build/WindowServer/WindowServer" \
    LD_LIBRARY_PATH="$PREFIX/lib:$BUILD/lib" \
    SERENITY_RES_ROOT="$SERENITY/Base/res" \
    "$SDE_ROOT/scripts/run-headless-test.sh" > "$RUNTIME/headless.log" 2>&1 || true
check "the prebuilt compositor passed the smoke test" \
    "grep -qF '==> R1 headless smoke test PASSED' '$RUNTIME/headless.log'"

if [[ "$fail" -ne 0 ]]; then
    echo "--- headless (tail) ---"
    tail -30 "$RUNTIME/headless.log" 2>/dev/null || true
    echo "==> split-framework test FAILED"
    exit 1
fi
echo "==> split-framework test PASSED"
