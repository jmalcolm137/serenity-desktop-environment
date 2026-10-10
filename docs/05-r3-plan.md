# Serenity Desktop Environment — R3 Plan (Serenity look & behaviours)

**Status:** Draft v0.1 · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`04-known-gaps.md`](04-known-gaps.md)

This document is the working plan for milestone **R3 — Serenity look &
behaviours** (`01-objectives.md` §11). It turns the R3 entries in
`04-known-gaps.md` into an ordered set of work items with mechanisms, the
SerenityOS reference code they mirror, and an exit/test for each.

---

## 1. Definition of done

R3 is complete when, per `01-objectives.md` §9.3–9.4 and the Phase 3 exit
criterion in `02-compositor-decision.md` §10:

1. the alt-tab switcher shows **live thumbnails** (Alt+Tab = current workspace,
   Super+Tab = all workspaces);
2. windows **snap** (Super+arrows and drag-to-edge), with the Serenity snap
   preview and geometry overlays;
3. **workspaces** switch across a grid of window stacks, and the unmodified
   Taskbar/WorkspacePicker reflect the current stack;
4. the Taskbar's **applet area** lays out the configured applets, in order, at
   the position the Taskbar supplies;
5. **notifications** appear as Serenity banners that are not part of the window
   list;
6. the shell's **tooltips** work (quick-launch, clock, window-title buttons,
   applets);
7. the result matches Serenity screenshots **structurally**.

Everything scriptable is covered by a headless test; menus, popups and IME stay
manual.

## 2. Current state

**Done (chrome).** SSD Serenity frames, per-client `xdg-decoration`, the
compositor-side window menu, fullscreen, Show Desktop, and theme-broker restyle
(see `README.md` and `04-known-gaps.md`).

**Stubs / no-ops in `WindowServer/src/main.cpp`:**

| Item | Where |
|---|---|
| `set_workspace(row, column)` | `WindowManagerServerConnection` (`main.cpp:2399`) |
| `set_applet_area_position(position)` | `main.cpp:2397` |
| `start_window_resize`, `popup_window_menu`, `set_window_taskbar_rect` | `main.cpp:2390–2392` |
| Workspace fields in `window_state_changed` | hardcoded `0, 0` (`main.cpp:2436`) |
| Super-key WMS events | never emitted |
| R3 keybindings | only Alt+Escape exists (`handle_keybinding`, `main.cpp:514`) |

**Missing entirely:** window switcher, snap overlays, workspace stacks,
notifications (NotificationServer stub), tooltips, fonts beyond bitmap Katica.

## 3. Foundation (do first)

The compositor is currently one 2,790-line `WindowServer/src/main.cpp`. R3 adds
four policy areas that SerenityOS keeps as separate classes. Before adding them:

- **Split policy into modules** mirroring Serenity: `WindowManager`
  (workspaces/tiling/focus policy), `WindowSwitcher`, `Overlays`,
  `AppletManager`, leaving `main.cpp` as wiring. Keep the D5 "wlroots behind a
  C++ platform layer" boundary (`03-engineering-conventions.md`).
- **Shared overlay/thumbnail infrastructure.** A way to draw LibGfx chrome into
  an offscreen `wlr_buffer` above all windows (the `BitmapBuffer` at
  `main.cpp:188` already does this for frames; overlays need a persistent scene
  node in `overlay_tree`), plus **toplevel snapshotting** for thumbnails.
- **Decide the capture path once, here:** composite a toplevel's scene subtree
  into a texture (simplest, no protocol) vs. `ext-image-copy-capture-v1`
  (wlroots support is vendored at
  `third_party/wlroots/types/wlr_ext_image_copy_capture_v1.c`). This is open
  question §12.5 of `02-compositor-decision.md`.

## 4. Work items

### WS1 — Workspaces (before the switcher)

Serenity models workspaces as a **grid of window stacks** (`WindowManager`:
default 2×2, max 16×16, from `[Workspaces]` in `WindowServer.ini`); each window
belongs to a stack, and only the current stack is visible.

- Add the stack grid to the compositor: `rows`/`columns` from
  `WindowServer.ini [Workspaces]`, per-`Toplevel` `(row, column)` (default to the
  current stack), current stack. Scene visibility = only the current stack's
  nodes enabled — the same mechanism `show_desktop` already uses
  (`main.cpp:2370`).
- Keybindings (Serenity `WindowManager.cpp:1753`): **Ctrl+Alt+arrows** switch
  stacks; **Ctrl+Alt+Shift+arrows** carry the focused window; briefly show
  `WindowStackSwitchOverlay` during the switch.
- `WindowManagerServer`: implement `set_workspace(row, column)`; emit
  `workspace_changed`; pack the real row/column into `window_state_changed`
  (currently `0, 0` at `main.cpp:2436`); honour `WMEventMask::WorkspaceChanges`.
- Emit the **Super-key WMS events** the Taskbar expects
  (`WindowManagerClient.ipc`): `super_key_pressed`, `super_space_key_pressed`,
  `super_d_key_pressed`, `super_digit_key_pressed`.
- Verify the unmodified **WorkspacePicker** applet
  (`Userland/Applets/WorkspacePicker`) renders and switches; export the stack
  dimensions if it needs them.

**Reference:** `Userland/Services/WindowServer/WindowManager.cpp` (window-stack
grid, keybindings), `WindowStackSwitchOverlay` in `Overlays.h`,
`WindowManagerServer.ipc` / `WindowManagerClient.ipc`.

### WS2 — Snapping (tiling) + overlays

Serenity's `WindowTileType` has Left/Right/Top/Bottom and corners
(`Window.h:45`); **Super+↑** maximizes, **Super+↓** minimizes, **Super+←/→**
tile (`WindowManager.cpp:1804+`); dragging to a screen edge tiles; the
`TileWindowOverlay` previews the target and `WindowGeometryOverlay` shows
geometry.

- Add `WindowTileType` to `Toplevel`; implement the Super+arrow semantics and
  `set_tiled`/`set_untiled` against the output usable area; propagate to the
  `xdg_toplevel` `tiled`/`maximized` states.
- **Drag-to-edge snapping**: during the interactive move (`process_cursor_move`),
  hit-test the pointer against screen edges, show `TileWindowOverlay`, and tile
  on release. Gate on the system-effects tile mode.
- Route `start_window_resize` through the existing `toplevel_begin_interactive`
  path instead of leaving it a stub.
- Draw the overlays with LibGfx in the top layer (foundation §3).

**Reference:** `WindowManager.cpp` (`show_tile_window_overlay`, move/release
handling), `WindowServer/Overlays.{h,cpp}` (`TileWindowOverlay`,
`WindowGeometryOverlay`).

### WS3 — Window switcher with live thumbnails

Compositor-side by decision — Serenity's `WindowSwitcher` is `WindowServer`
code (`02-compositor-decision.md` §7 and open question §12.7).

- Trigger: **Alt+Tab** = current workspace, **Super+Tab** = all workspaces,
  Shift reverses (`WindowManager.cpp:1736`). Add a keyboard grab while visible
  (the pattern used by the open window menu, `main.cpp:538`) and activate the
  selection on modifier release.
- Draw the Serenity layout from `WindowSwitcher.cpp`: a list of
  {thumbnail 64×64, icon, title, geometry, `row:col` when spanning stacks},
  selection highlight, mouse hover/click, using the theme palette.
- **Live thumbnails**: snapshot each toplevel when the switcher opens and keep a
  cached last frame per toplevel (Serenity keeps the backing store) so minimized
  windows still show content; refresh on damage while open.
- Selecting a window focuses it and, in all-workspaces mode, switches to its
  stack.

**Reference:** `Userland/Services/WindowServer/WindowSwitcher.{h,cpp}`,
`WindowManager::set_highlight_window`.

### WS4 — Applet area / system tray parity

Serenity's `AppletManager` lays applets right-to-left in an applet area, sizes
it, and warns the Taskbar via `applet_area_size_changed`; the Taskbar replies
with `set_applet_area_position` (`Taskbar/TaskbarWindow.cpp:208`,
`AppletManager.cpp:94`). The order comes from `[Applet] Order=…`.

- Implement `set_applet_area_position` and emit `applet_area_size_changed`; lay
  applets out in the configured order.
- **Decide the placement channel.** Today `libgui-wayland` anchors every applet
  bottom-right for itself (`../libgui-wayland/src/WaylandClient.cpp:1620`), so
  multiple applets collide. Either the compositor offsets the applet layer
  surfaces from the Taskbar-supplied area, or extend `serenity-window.xml` so
  LibWindowServer asks the compositor to position applet surfaces. The protocol
  extension is cleaner under P2 (the host change stays in libgui-wayland).
- Honour `[Applet] Order`; forward input so hover/press work (supports WS6).

**Reference:** `Userland/Services/WindowServer/AppletManager.cpp`,
`Base/etc/WindowServer.ini` (`[Applet] Order`, `[Workspaces]`),
`Userland/Services/Taskbar/TaskbarWindow.cpp`.

### WS5 — Notifications

`NotificationServer` is a stub today. The protocol is
`Userland/Services/NotificationServer/NotificationServer.ipc`
(`show_notification`, `update_notification_text/icon/launch_url`, `is_showing`,
`close_notification`); clients use `LibGUI/Notification`.

- **Preferred (maximum parity):** run the **unmodified SerenityOS
  `NotificationServer`** as a session client served on the portal bus, and give
  the compositor `WindowType::Notification` semantics — undecorated banner,
  top-right, above normal windows, excluded from the window list and switcher,
  click launches the URL, auto-dismiss.
- **Fallback:** a thin SDE `NotificationServer` shim plus a small banner client,
  following the service-shim pattern.
- Feasibility hinges on the server's dependence on
  `Desktop::the().system_effects()` and window-type plumbing; **spike this
  first** (precedent: `WebContent` is deferred for the same class of reason,
  `04-known-gaps.md`).

**Reference:** `Userland/Services/NotificationServer/`,
`Userland/Libraries/LibGUI/Notification.cpp`.

### WS6 — Tooltips

`GUI::Application::show_tooltip()` is a no-op in the port
(`04-known-gaps.md`), so the Taskbar quick-launch, clock, window-title buttons
and applets show no hover text.

- **Host (libgui-wayland):** wire `show_tooltip` to a `WindowType::Tooltip`
  surface, using the timing `Application` already implements
  (`LibGUI/Application.cpp:157`).
- **Compositor:** give `WindowType::Tooltip` semantics — undecorated, always on
  top, non-focusable, positioned near the cursor/requested point, excluded from
  the window list and switcher; follow the cursor while visible.
- Note: **window-title button tooltips are compositor chrome**, so the
  compositor draws those itself; client tooltips arrive through the protocol.
  Placement may need a small `serenity-window.xml` extension, as with WS4.

**Reference:** `Userland/Libraries/LibGUI/Application.cpp`
(`show_tooltip`/`TooltipWindow`).

### WS7 — Font parity / size-based zoom (decision only)

Fixed to bitmap Katica; `set_font_size` resolves to nothing, so zoom is a no-op
(matches upstream — `02-compositor-decision.md` §11). The real fix needs a
scalable face (`SerenitySans-Regular.ttf`) and a font selector, which is
**R4/G5**, not R3. For R3: **confirm and document the parity decision**; do not
block the milestone on it.

## 5. Sequencing

1. **Foundation** — module split + overlay/thumbnail/snapshot infrastructure.
2. **WS1 workspaces** — unblocks switcher "all windows", WorkspacePicker, and
   the Super-key WMS events.
3. **WS2 snapping + overlays** — shares the overlay infrastructure.
4. **WS3 switcher + thumbnails** — shares both.
5. **WS4 applet area** — independent; needs the placement-channel decision.
6. **WS5 notifications** — independent; feasibility spike first.
7. **WS6 tooltips** — cross-repo; independent.
8. **WS7** — decision only.

## 6. Test strategy

- Extend `scripts/run-headless-test.sh` (keep the R1 checks) with **virtual
  input** (`wlr-virtual-keyboard` / `wlr-virtual-pointer`, already in the
  protocol target list of `02-compositor-decision.md` §13) to script Alt+Tab,
  Super+arrows, Ctrl+Alt+arrows, and assert through compositor log lines plus
  pixel snapshots.
- Notifications and tooltips get small scripted clients.
- Menus, popups and IME remain manual verification.

## 7. Risks and decisions

| Risk / decision | Note | Mitigation |
|---|---|---|
| **Thumbnail capture path** | scene-render vs `ext-image-copy-capture`; perf at open | Decide in foundation §3; cache last frame per toplevel |
| **Unmodified NotificationServer reuse** | depends on `system_effects()` and window-type plumbing | Spike first; fall back to a shim + banner client |
| **Applet placement channel** | compositor offsets vs `serenity-window` extension | Prefer the protocol extension under P2 |
| **Tooltips need a host change** | in `libgui-wayland`, not this repo | Cross-repo coordination; small, upstreamable |
| **`main.cpp` refactor scope** | recommended, not strictly required | Time-box; keep behaviour identical while splitting |
| **Font-size zoom** | needs a scalable face + settings UI | Accept parity; defer the fix to R4/G5 |
