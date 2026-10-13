# R5/R6 — Protocol parity & the wlroots version plan

**Status:** living · **Updated:** 2026-10-13
**Related:** [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §11 (the KDE-parity
protocol table) · [`20-xwayland-next-steps.md`](20-xwayland-next-steps.md)

This records **how SDE tracks wlroots** for the standard-protocol parity work
(R5/R6), and the concrete order in which the remaining protocol rows land.

## 1. Policy: pin the wlroots *release*, the one Arch ships

SDE builds against a **pinned wlroots release** (`scripts/fetch-wlroots.sh`,
currently **0.20.2**), not the `master` branch. This matches what **Arch Linux**
ships — `wlroots0.20` 0.20.2, updated 2026-07-10, three days after the upstream
release on 2026-07-07 — so the AUR/native session (`docs/13`) links a version
distros actually have. We do **not** chase `master`.

wlroots releases a **minor** roughly every 10 months (`0.19.0` 2025-05-15 →
`0.20.0` 2026-03-26), with patch releases between. `master` is now `0.21.0-dev`;
the `0.21.0` release is expected **late 2026 / early 2027**, and Arch picks it up
within days of release.

## 2. Availability tiers (what unblocks what)

Diffing `master` (0.21-dev) against `0.20.2`, and checking the pinned headers:

### (a) Available now on 0.20.2 — implement without any bump

| Protocol | API present in 0.20.2 |
|---|---|
| `xdg-toplevel-icon-v1` | `wlr_xdg_toplevel_icon_manager_v1_create()` |
| `xdg-system-bell-v1` | `wlr_xdg_system_bell_v1_create()` |

### (b) Lands in wlroots **0.21** — wait for the release, then bump

| Protocol | Notes |
|---|---|
| **XDND** (XWayland drag-and-drop) | `xwayland/selection/dnd.c`; wired into the seat's drag, so **no compositor changes** once present. `docs/20` item 9. |
| `ext-background-effect-v1` | new type header. |
| `wp-pointer-warp-v1` | new type header. |

### (c) Not in 0.20 **or** master (effectively blocked)

`xdg-toplevel-drag`, `wp-fifo-v1`, `xwayland-keyboard-grab-v1`.

## 3. Order of work

1. **Now (0.20.2):** implement `xdg-toplevel-icon-v1` (Wayland clients' window
   icons — pairs with the X11 `_NET_WM_ICON` work in `docs/20` item 3) and
   `xdg-system-bell-v1` (advertise the global; ring handling).
2. **When wlroots 0.21 releases (≈ early 2027, Arch within days):** bump the pin
   from 0.20.2 to 0.21 and pick up **XDND + `ext-background-effect-v1` +
   `wp-pointer-warp-v1`** together. Re-check the parity table then.
3. Re-evaluate tier (c) against whatever 0.21 (and later) actually ships.

This keeps SDE aligned with distributions while still clearing the parity table
as each upstream capability becomes releasable.
