#!/usr/bin/env bash
# Headless test for the XDG User Directories support: Core::StandardPaths must
# read $XDG_CONFIG_HOME/user-dirs.dirs (with $HOME expansion), keep the
# environment override, and fall back to $HOME/<Name>.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

if [[ "${SDE_TEST_NO_BUILD:-0}" != "1" ]]; then
    "$SDE_ROOT/scripts/build-tests.sh" libwm-user-dirs-test >/dev/null 2>&1 || true
fi

echo "=== xdg user dirs test ==="
env LD_LIBRARY_PATH="$BUILD/lib" "$SDE_ROOT/build/test/libwm-user-dirs-test"
echo "==> user dirs test PASSED"
