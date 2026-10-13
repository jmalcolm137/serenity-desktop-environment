#!/usr/bin/env bash
# Native-app protocol matrix test (docs/08 §3, docs/11 §11): assert the compositor
# advertises the compatibility protocol surface, and that the R5 additions are
# present. Runs headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIBGUI_WAYLAND_ROOT="${LIBGUI_WAYLAND_ROOT:-$SDE_ROOT/../libgui-wayland}"
SERENITY="${SERENITY:-$LIBGUI_WAYLAND_ROOT/serenity}"
BUILD="${BUILD:-$LIBGUI_WAYLAND_ROOT/build/lagom}"
COMP="$SDE_ROOT/build/WindowServer/WindowServer"
TOOLS_BUILD="$SDE_ROOT/build/protocol-matrix-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
cc -O2 "$SDE_ROOT/tools/wayland-globals-test.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/wayland-globals-test"

RUNTIME="$(mktemp -d "${TMPDIR:-/tmp}/sde-proto.XXXXXX")"
chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
COMP_PID=""
cleanup() {
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    # D8 (docs/03): unlink the compositor socket(s) this run created.
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
    rm -rf "$RUNTIME"
}
trap cleanup EXIT

SOCK="sde-proto-$$"
env -u WAYLAND_DISPLAY -u DISPLAY \
    WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
    SDE_WM_PORTAL_PATH="$RUNTIME/wm" \
    SERENITY_RES_ROOT="${SERENITY_RES_ROOT:-$SERENITY/Base/res}" \
    LD_LIBRARY_PATH="$BUILD/lib" \
    "$COMP" -n "$SOCK" > "$RUNTIME/comp.log" 2>&1 &
COMP_PID=$!
for _ in $(seq 1 80); do
    [[ -e "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$SOCK" ]] && break
    sleep 0.1
done
sleep 0.4

env WAYLAND_DISPLAY="$SOCK" "$TOOLS_BUILD/wayland-globals-test" > "$RUNTIME/globals.log" 2>&1 || true

echo "=== advertised globals ==="
sed 's/^/  /' "$RUNTIME/globals.log"

fail=0
check() {
    if grep -qE " ${1} v[0-9]" "$RUNTIME/globals.log"; then
        echo "  ok: $1"
    else
        echo "  MISSING: $1"
        fail=1
    fi
}

echo "=== core / shell ==="
check wl_compositor
check wl_subcompositor
check wl_seat
check wl_shm
check wl_data_device_manager
check xdg_wm_base
check zwlr_layer_shell_v1
check zxdg_decoration_manager_v1
check zwlr_foreign_toplevel_manager_v1
check zxdg_output_manager_v1
check xdg_activation_v1

echo "=== R5 native-app compatibility (this batch) ==="
check zwp_primary_selection_device_manager_v1
check wp_viewporter
check wp_content_type_manager_v1
check wp_tearing_control_manager_v1
check zwp_relative_pointer_manager_v1
check zwp_keyboard_shortcuts_inhibit_manager_v1
check zxdg_exporter_v1
check zxdg_importer_v1
check zxdg_exporter_v2
check zxdg_importer_v2

echo "=== R6 additions (quick wins + presentation-time) ==="
check wp_single_pixel_buffer_manager_v1
check wp_alpha_modifier_v1
check xdg_wm_dialog_v1
check wp_presentation
check wp_fractional_scale_manager_v1

echo "=== portals / capture / session (pre-existing) ==="
check wl_output
check zwp_idle_inhibit_manager_v1
check ext_idle_notifier_v1
check zwlr_output_power_manager_v1
check ext_session_lock_manager_v1
check ext_workspace_manager_v1
check ext_foreign_toplevel_list_v1
check ext_output_image_capture_source_manager_v1
check ext_image_copy_capture_manager_v1
check zwlr_screencopy_manager_v1

if [[ "$fail" -ne 0 ]]; then
    echo "==> protocol matrix test FAILED"
    exit 1
fi
echo "==> protocol matrix test PASSED"
