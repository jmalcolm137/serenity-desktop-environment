#!/usr/bin/env bash
# Native-app protocol matrix test (docs/08 §3, docs/11 §11): assert the compositor
# advertises the compatibility protocol surface, and that the R5 additions are
# present. Runs headless.
set -euo pipefail

SDE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$SDE_ROOT/scripts/lib/testlib.sh"
sde_test_init protocol-matrix
sde_test_export_roots
TOOLS_BUILD="$SDE_ROOT/build/protocol-matrix-test"

"$SDE_ROOT/scripts/build-window-server.sh" >/dev/null

mkdir -p "$TOOLS_BUILD"
cc -O2 "$SDE_ROOT/tools/wayland-globals-test.c" \
    $(pkg-config --cflags --libs wayland-client) \
    -o "$TOOLS_BUILD/wayland-globals-test"

chmod 700 "$RUNTIME"
export SDE_STATE_DIR="$RUNTIME"
cleanup() {
    [[ -n "$COMP_PID" ]] && kill "$COMP_PID" 2>/dev/null || true
    [[ -n "$COMP_PID" ]] && wait "$COMP_PID" 2>/dev/null || true
    # D8 (docs/03): unlink the compositor socket(s) this run created.
    rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$* "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/sde-*-$$*.lock 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${COMP_PID:-${CP:-}}" 2>/dev/null || break; sleep 0.05; done; rm -rf "$RUNTIME"
}

sde_test_compositor SDE_WM_PORTAL_PATH="$RUNTIME/wm"

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
check zwlr_data_control_manager_v1
check ext_data_control_manager_v1
check wp_viewporter
check wp_content_type_manager_v1
check wp_tearing_control_manager_v1
check zwp_relative_pointer_manager_v1
check zwp_pointer_constraints_v1
check zwp_pointer_gestures_v1
check zwp_tablet_manager_v2
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

echo "=== R6 protocol catch-up (colour, security, tag, sync, lease) ==="
check xdg_toplevel_tag_manager_v1
check xdg_toplevel_drag_manager_v1
check wp_color_representation_manager_v1
check wp_security_context_manager_v1
check wp_pointer_warp_v1
if grep -q 'color management enabled' "$RUNTIME/comp.log"; then
    check wp_color_manager_v1
else
    echo "  skip: wp_color_manager_v1 (renderer has no input color transform)"
fi
if grep -q 'explicit sync enabled' "$RUNTIME/comp.log"; then
    check wp_linux_drm_syncobj_manager_v1
else
    echo "  skip: wp_linux_drm_syncobj_manager_v1 (no DRM fd)"
fi
if grep -q 'DRM lease enabled' "$RUNTIME/comp.log"; then
    check wp_drm_lease_device_v1
else
    echo "  skip: wp_drm_lease_device_v1 (no DRM backend)"
fi

echo "=== standard-protocol parity (docs/21-r5-protocol-parity-plan.md) ==="
check xdg_toplevel_icon_manager_v1
check xdg_system_bell_v1
# R5 IME: text-input-v3 (clients) + input-method-v2 (input methods) (docs/22).
check zwp_text_input_manager_v3
check zwp_input_method_manager_v2

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
