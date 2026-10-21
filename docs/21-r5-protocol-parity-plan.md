# R5/R6: Protocol parity & the wlroots version plan

**Status:** living · **Updated:** 2026-10-20
**Related:** [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §11 (the KDE-parity
protocol table) · [`20-xwayland-next-steps.md`](20-xwayland-next-steps.md)

This records **how SDE tracks wlroots** for the standard-protocol parity work
(R5/R6), and the concrete order in which the remaining protocol rows land.

## 1. Policy: pin the wlroots *release*, the one Arch ships

SDE builds against a **pinned wlroots release** (`scripts/fetch-wlroots.sh`,
currently **0.20.2**), not the `master` branch. This matches what **Arch Linux**
ships (`wlroots0.20` 0.20.2, updated 2026-07-10, three days after the upstream
release on 2026-07-07), so the AUR/native session (`docs/13`) links a version
distros actually have. We do **not** chase `master`.

wlroots releases a **minor** roughly every 10 months (`0.19.0` 2025-05-15 →
`0.20.0` 2026-03-26), with patch releases between. `master` is now `0.21.0-dev`;
the `0.21.0` release is expected **late 2026 / early 2027**, and Arch picks it up
within days of release.

## 2. Availability tiers (what unblocks what)

Diffing `master` (0.21-dev) against `0.20.2`, and checking the pinned headers:

### (a) Available now on 0.20.2: implement without any bump

| Protocol | API present in 0.20.2 |
|---|---|
| `xdg-toplevel-icon-v1` | `wlr_xdg_toplevel_icon_manager_v1_create()` |
| `xdg-system-bell-v1` | `wlr_xdg_system_bell_v1_create()` |
| `wp-pointer-warp-v1` | no wlroots manager; served by the compositor directly (`src/Pointer.cpp`) |
| `xdg-toplevel-drag-v1` | no wlroots manager; served by the compositor directly (`src/XdgToplevelDrag.cpp`) |
| `wp-color-representation-v1` | `wlr_color_representation_manager_v1_create_with_renderer()` |
| `wp-color-management-v1` | `wlr_color_manager_v1_create()` (the renderer must expose input colour transforms; wlroots' Vulkan renderer does, GLES2 does not) |
| `wp-security-context-v1` | `wlr_security_context_manager_v1_create()` |
| `xdg-toplevel-tag-v1` | `wlr_xdg_toplevel_tag_manager_v1_create()` |
| `wp-linux-drm-syncobj-v1` | `wlr_linux_drm_syncobj_manager_v1_create()` (needs a DRM fd) |
| `wp-drm-lease-v1` | `wlr_drm_lease_v1_manager_create()` (needs a DRM backend) |

### (b) Lands in wlroots **0.21**: wait for the release, then bump

| Protocol | Notes |
|---|---|
| **XDND** (XWayland drag-and-drop) | `xwayland/selection/dnd.c`; wired into the seat's drag, so **no compositor changes** once present. `docs/20` item 9. |
| `ext-background-effect-v1` | new type header. |
| `wp-fifo-v1` | frame-pacing barriers. |
| `wp-commit-timing-v1` | client-specified commit timestamps. |

### (c) Not in 0.20 **or** master (effectively blocked)

`xwayland-keyboard-grab-v1`.

## 3. Order of work

1. ~~**Now (0.20.2):** implement `xdg-toplevel-icon-v1` (Wayland clients' window
   icons) and `xdg-system-bell-v1`.~~ **Done** (`e47d098`): both globals are served;
   the icons pair with the X11 `_NET_WM_ICON` work in `docs/20` item 3.
2. **When wlroots 0.21 releases (≈ early 2027, Arch within days):** bump the pin
   from 0.20.2 to 0.21 and pick up **XDND + `ext-background-effect-v1` +
   `wp-fifo-v1` + `wp-commit-timing-v1`** together.
   Re-check the parity table then.
3. **Landed on 0.20.2 (2026-10-20):** colour representation,
   colour management (Vulkan-only), security-context, toplevel-tag, explicit sync
   (DRM), DRM lease (DRM), pointer-warp and xdg-toplevel-drag.
4. Re-evaluate tier (c) against whatever 0.21 (and later) actually ships.

This keeps SDE aligned with distributions while still clearing the parity table
as each upstream capability becomes releasable.
