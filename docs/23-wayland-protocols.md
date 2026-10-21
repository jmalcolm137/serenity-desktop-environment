# SDE: the Wayland protocols it implements

**Status:** living · **Updated:** 2026-10-20
**Related:** [`21-r5-protocol-parity-plan.md`](21-r5-protocol-parity-plan.md) (the standard-protocol
parity plan and the wlroots version policy) · [`04-known-gaps.md`](04-known-gaps.md)

This is the list of Wayland protocols the SDE compositor (`WindowServer`) **advertises and
supports**: every global an unmodified client can bind. The compositor is wlroots-based, so a
global is normally created by wlroots and *consumed* by SDE; where SDE supplies the missing
policy (cursor constraints, layer arrangement, decoration negotiation, the X11 shell surface,
…) that is noted in [`04-known-gaps.md`](04-known-gaps.md) and
[`08-compositor-gaps.md`](08-compositor-gaps.md).

The list is not aspirational: it is the exact set of globals a fresh headless compositor
advertises. It is emitted by `tools/wayland-globals-test.c` and the compatibility subset is
asserted by `scripts/run-protocol-matrix-test.sh` (the full dump is printed by that test).

## Core

| Protocol | Advertised | Purpose |
|---|---|---|
| `wl_compositor` | v5 | surfaces, regions, the core surface role plumbing |
| `wl_subcompositor` | v1 | sub-surfaces |
| `wl_shm` | v2 | shared-memory buffers |
| `wl_seat` | v9 | input seats (keyboard / pointer / touch), capabilities |
| `wl_output` | v4 | outputs, modes, scale, geometry |
| `zwp_linux_dmabuf_v1` | v4 | DMA-BUF buffer import (zero-copy) |

## Shell & window management

| Protocol | Advertised | Purpose |
|---|---|---|
| `xdg_wm_base` | v3 | the standard `xdg-shell` toplevel/popup surface roles |
| `xdg_wm_dialog_v1` | v1 | mark a toplevel as a modal dialog |
| `zwlr_layer_shell_v1` | v4 | layer surfaces (Taskbar, applets, Desktop, notifications, tooltips) |
| `zxdg_decoration_manager_v1` | v1 | server-side decoration negotiation |
| `xdg_toplevel_icon_v1` | v1 | client-provided window icons (`xdg-toplevel-icon-v1`) |
| `xdg_activation_v1` | v1 | activation tokens (taskbar/portal "activate this window") |
| `xdg_system_bell_v1` | v1 | the system bell (global advertised; no audible output yet) |
| `zwlr_foreign_toplevel_manager_v1` | v3 | taskbar-style task list from an external client |
| `xdg_toplevel_tag_manager_v1` | v1 | per-toplevel tag and description, for window rules |

## Input

| Protocol | Advertised | Purpose |
|---|---|---|
| `zwp_relative_pointer_manager_v1` | v1 | relative pointer motion (games, pointer lock) |
| `wp_pointer_warp_v1` | v1 | warp the pointer to a surface-local position (implemented by SDE) |
| `zwp_pointer_constraints_v1` | v1 | pointer lock / confinement, **enforced by SDE** |
| `zwp_pointer_gestures_v1` | v3 | swipe / pinch / hold gestures |
| `zwp_tablet_manager_v2` | v1 | graphics tablet tools + tablet pads |
| `zwp_keyboard_shortcuts_inhibit_manager_v1` | v1 | let a client (e.g. a VM) grab shortcuts |
| `wp_cursor_shape_manager_v1` | v1 | named cursor shapes |
| `zwp_text_input_manager_v3` | v1 | text input (text-input-v3); see [`22-input-method.md`](22-input-method.md) |
| `zwp_input_method_manager_v2` | v1 | input-method-v2 (on-screen / CJK input methods) |
| `zwp_virtual_keyboard_manager_v1` | v1 | synthetic keyboard input |
| `zwlr_virtual_pointer_manager_v1` | v2 | synthetic pointer input |

## Selection & data transfer

| Protocol | Advertised | Purpose |
|---|---|---|
| `wl_data_device_manager` | v3 | drag-and-drop + the `CLIPBOARD` selection |
| `zwp_primary_selection_device_manager_v1` | v1 | the `PRIMARY` selection |
| `zwlr_data_control_manager_v1` | v2 | clipboard managers (`wlr-data-control`) |
| `ext_data_control_manager_v1` | v1 | clipboard managers (`ext-data-control-v1`) |

## Session, idle, lock & outputs

| Protocol | Advertised | Purpose |
|---|---|---|
| `xdg_session_manager_v1` | v1 | session management / restore |
| `ext_idle_notifier_v1` | v2 | idle notification (idle-to-lock, DPMS) |
| `zwp_idle_inhibit_manager_v1` | v1 | inhibit idle (video playback, etc.) |
| `ext_session_lock_manager_v1` | v1 | session locking |
| `zwlr_output_manager_v1` | v4 | output configuration (Display Settings) |
| `zwlr_output_power_manager_v1` | v1 | output power / DPMS |
| `wp_security_context_manager_v1` | v1 | sandboxed-client identification (Flatpak, …) |
| `wp_linux_drm_syncobj_manager_v1` | v1 | explicit synchronization (DRM renderers) |
| `wp_drm_lease_device_v1` | v1 | DRM leasing (DRM backends; the primary output is not offered) |

## Colour

| Protocol | Advertised | Purpose |
|---|---|---|
| `wp_color_representation_manager_v1` | v1 | per-surface colour representation (alpha mode, coefficients, range) |
| `wp_color_manager_v1` | v1 | colour management; advertised only when the renderer has input colour transforms (Vulkan), because GLES2/pixman cannot perform them |

## Workspaces & foreign toplevels

| Protocol | Advertised | Purpose |
|---|---|---|
| `ext_workspace_manager_v1` | v1 | workspaces to external clients (WorkspacePicker) |
| `ext_foreign_toplevel_list_v1` | v1 | a window list to external clients |

## Capture & screencopy

| Protocol | Advertised | Purpose |
|---|---|---|
| `ext_output_image_capture_source_manager_v1` | v1 | output capture sources |
| `ext_image_copy_capture_manager_v1` | v1 | image-copy capture (ScreenCast portal) |
| `ext_foreign_toplevel_image_capture_source_manager_v1` | v1 | capture a foreign toplevel |
| `zwlr_screencopy_manager_v1` | v3 | legacy screencopy |

## XWayland

| Protocol | Advertised | Purpose |
|---|---|---|
| `xwayland_shell_v1` | v1 | associate X11 windows with Wayland surfaces; see [`20-xwayland-next-steps.md`](20-xwayland-next-steps.md) |

## SDE-specific extensions

| Protocol | Advertised | Purpose |
|---|---|---|
| `serenity_window_manager` | v1 | the compositor role of the SerenityOS WindowServer protocol used by the shell apps (window list, icons, applet area, chrome) |
| `serenity_virtual_input_manager_v1` | v1 | test-only input injection for the headless harness (touch/pen) |

## Deliberately absent (blocked upstream)

These are **not** advertised, and are tracked in
[`21-r5-protocol-parity-plan.md`](21-r5-protocol-parity-plan.md):

- **wlroots 0.21 / not in the pinned 0.20.2:** XWayland drag-and-drop (XDND,
  `xwayland/selection/dnd.c`), `ext-background-effect-v1`, `wp-fifo-v1`,
  `wp-commit-timing-v1`.
- **In no wlroots release:** `xwayland-keyboard-grab-v1`, `xdg-toplevel-drag`.

The compositor pins the wlroots **release** that Arch Linux ships (currently `0.20.2`); these
land when the pin moves to the 0.21 release (expected late 2026 / early 2027).
