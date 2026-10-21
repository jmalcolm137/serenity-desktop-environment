#!/usr/bin/env bash
# ci-tier: full
# Headless colour-management test. wlroots 0.20 only exposes input colour
# transforms on its Vulkan renderer, so start the compositor with it and assert
# the standard colour protocols (wp-color-management-v1 and
# wp-color-representation-v1) are advertised. Skips when the host has no working
# Vulkan renderer; the compositor then falls back to pixman, where omitting
# colour management is the correct behaviour.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init color-management
sde_test_export_roots
TOOLS_BUILD="$SDE_ROOT/build/color-management-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null
mkdir -p "$TOOLS_BUILD"
cc -O2 "$SDE_ROOT/Tools/wayland-globals-test.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/wayland-globals-test"

sde_test_compositor SDE_RENDERER=vulkan SDE_WM_PORTAL_PATH="$RUNTIME/wm"

env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/wayland-globals-test" > "$RUNTIME/globals.log" 2>&1 || true

echo "=== renderer ==="
grep -aE "renderer '|color management" "$RUNTIME/comp.log" | head -5 || true
if ! grep -q 'color management enabled' "$RUNTIME/comp.log"; then
    echo "SKIP: no Vulkan renderer with input colour transforms on this host"
    sde_test_case_stop
    exit 0
fi

echo "=== colour protocols ==="
sde_test_check "$RUNTIME/globals.log" 'wp_color_manager_v1' 'wp-color-management-v1 is advertised'
sde_test_check "$RUNTIME/globals.log" 'wp_color_representation_manager_v1' 'wp-color-representation-v1 is advertised'

# Stop before checking for shutdown faults: the tag and security-context
# managers assert their signal lists are empty when the display is destroyed.
sde_test_case_stop
sde_test_no_fault "$RUNTIME/comp.log"
sde_test_finish "$RUNTIME/globals.log"
