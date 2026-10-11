#!/usr/bin/env bash
# Headless test for the session /bin shadow (R4.2, docs/11): the port's
# Process::resolve_executable_path() must consult SDE_SESSION_BIN_DIR, so an
# unmodified SerenityOS app spawning /bin/shutdown (the Taskbar "Exit..." menu)
# reaches the session's sde-power helper. Needs no compositor.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"

echo "== building prerequisite =="
ninja -C "$BUILD" libwm-binpath-test >/dev/null

echo "=== binpath (/bin shadow) test ==="
env LD_LIBRARY_PATH="$BUILD/lib" "$BUILD/bin/libwm-binpath-test"
echo "==> binpath test PASSED"
