# Serenity Desktop Environment: R3 Implementation Plan

**Status:** Draft v0.1 · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`04-known-gaps.md`](04-known-gaps.md) · [`05-r3-plan.md`](05-r3-plan.md) · [`07-systemserver-roles.md`](07-systemserver-roles.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md) · [`10-font-parity-decision.md`](10-font-parity-decision.md)

[`05-r3-plan.md`](05-r3-plan.md) defines **what** R3 is: the work items, the
SerenityOS reference code, and an exit test for each. This document is its
execution companion: **in what order and how** to build it, what to de-risk
first, and what to deliberately leave alone. Keep the two in sync: 05 owns scope,
this document owns sequencing and the critical path.

---

## 1. Where R3 stands

- R0, R1 and R2 are done; R2's loose ends are closed or explicitly reassigned
  (see `04-known-gaps.md`, "R2 follow-up").
- The R3 **foundation** (§2) has landed, both **spikes** (§3) are resolved, and
  **WS1 workspaces**, **WS2 snapping**, **WS3 switcher**, **WS4 applet area**,
  **WS5 notifications** and **WS6 tooltips** are complete. **WS7 (font parity)** is
  a recorded decision ([`10-font-parity-decision.md`](10-font-parity-decision.md)):
  R3 keeps the bitmap Katica family and accepts no-op zoom, deferring scalable
  fonts to R4/G5. **R3 is complete.**

| Step | Status |
|---|---|
| Foundation: module split, overlays, config, input harness (§2) | **Done** |
| Spikes: thumbnail capture, NotificationServer reuse (§3) | **Done** |
| WS1: workspaces | **Done** |
| WS2: snapping + overlays | **Done** |
| WS3: switcher | **Done** |
| WS4: applet area | **Done** |
| WS5: notifications | **Done** |
| WS6: tooltips | **Done** |
| WS7: font parity | **Decided** (docs/10) |

**Strategy (unchanged):** finish R3 before taking on R4+ breadth. The service
scaffolding (`LaunchServer`, `ImageDecoder`, the layering/SystemServer-role work)
is sufficient; stop adding services or shell polish until the behaviours land.

## 2. Foundation (landed)

Every remaining R3 feature lands in a policy area SerenityOS keeps as its own
class. That structure is now in place, so features are no longer written into the
monolith:

1. **Module split: done.** `Overlays`, `WindowManager` and the shared `Model` are
   out of `main.cpp`
   (`Services/WindowServer/src/{Overlays,WindowManager,Config,Model}.{h,cpp}`); `main.cpp`
   is now wiring plus the policies not yet extracted. `WindowSwitcher` (WS3) and
   `AppletManager` (WS4) have landed.
2. **Overlay + snapshot infrastructure: done.** `ChromeNode` renders LibGfx
   chrome into a scene buffer in `overlay_tree`; `snapshot_toplevel()` reads a
   toplevel's client buffer into a `Gfx::Bitmap` (spike §3). Shared by snapping
   and the switcher.
3. **Compositor config: done.** `Config` reads `WindowServer.ini`
   (`[Workspaces]`, `[Applet] Order`, …) with the SerenityOS defaults overlaid by
   the user's file under `$XDG_CONFIG_HOME/sde`, plus live reload.
4. **Scriptable tests: done.** Virtual keyboard/pointer in the headless harness
   (`scripts/run-input-test.sh`), asserting the window menu, workspace switching
   and thumbnail capture.

The `main.cpp` monolith the plan warned about is gone; `WindowSwitcher` (WS3) was
extracted with its feature, and `AppletManager` (WS4) has too.

## 3. De-risk before committing (time-boxed spikes)

Two unknowns decide how the features are built. Resolve them before the features
that depend on them, each with a clear exit criterion and a fallback.

| Spike | Question | Exit / fallback |
|---|---|---|
| **Thumbnail capture** | Composite the toplevel scene subtree into a buffer, or use `ext-image-copy-capture`? | A cached per-toplevel thumbnail that renders in the switcher; fallback to icon + stale frame. Decides WS3's implementation. |
| **NotificationServer reuse** | Does the unmodified SerenityOS `NotificationServer` run as a session client under `WindowType::Notification`, or do we need a shim + banner client? | A banner appears with correct window-type semantics; fallback to the service-shim pattern. |

Both are **resolved** (outcomes below) and recorded as closed in §8; they were
scheduled as prerequisites rather than late surprises.

### Spike outcomes

- **Thumbnail capture: resolved (prototype).** Read the client's submitted buffer
  back directly with `wlr_buffer_begin_data_ptr_access()`; our clients submit
  `wl_shm` buffers, so this is a straight copy into a `Gfx::Bitmap`
  (`Services/WindowServer/src/Overlays.cpp`, `snapshot_toplevel()`). A non-mappable
  (DMA-BUF) buffer returns `nullptr` and callers fall back to the app icon;
  `ext-image-copy-capture-v1` or a scene-subtree render would lift that later.
  Verified headlessly: `scripts/run-input-test.sh` captures a 320×200 toplevel and
  dumps a PNG. WS3 keeps the cached per-toplevel bitmap and refreshes it on damage.
- **NotificationServer reuse: resolved (reuse).** The unmodified SerenityOS
  `NotificationServer` builds (deps are all available; its IPC endpoints are
  already generated by the Lagom build) and hosts as an `IPC::MultiServer` on
  `/portal/notify`, exactly like `SQLServer` (`LibWM::spawn_multi_client_service`).
  It does **not** touch `Desktop::system_effects()`. The only gap was window-type
  plumbing: the shim now maps `WindowType::Notification` to an undecorated overlay
  layer surface at the client's position (`serenity-notification`), so a
  notification is a banner rather than a decorated window and stays out of the
  window list. Verified: the notify portal spawns the server and the compositor
  creates the `serenity-notification` layer surface. **Completed (WS5):** the
  remaining client failure was misdiagnosed as an IPC *encoding* fault; the real
  cause is that `LibGUI::Notification::show()` makes a **synchronous** IPC call
  whose incoming messages are dispatched through the current `Core::EventLoop`, so
  an event loop (a `GUI::Application`) must already exist. The reused server was
  then exercised end-to-end (icon/no-icon, launch URL, update) by
  `scripts/run-notification-test.sh`; see §4 WS5.

## 4. Feature order (dependency-driven)

```
        foundation (§2) + spikes (§3)
                  │
      ┌───────────┼───────────────┬───────────────┐
      ▼           ▼               ▼               ▼
 workspaces   snapping/overlays  applet area   notifications
      │           │                              (independent)
      └─────┬─────┘
            ▼
        switcher  ← needs workspaces (all-windows mode) + overlays
                        │
                        ▼
                     tooltips  (cross-repo; independent)
```

1. **Workspaces (WS1): done.** The stack grid, `set_workspace`/
   `workspace_changed`, the real `(row, column)` in `window_state_changed`,
   Ctrl+Alt+arrow switching, the Super-key WMS events, and the verified
   WorkspacePicker applet.
2. **Snapping + overlays (WS2): done.** `WindowTileType`, Super+arrow
   maximize/minimize/tile (plus Super+Alt horizontal/vertical maximize)
   propagating `xdg_toplevel` tiled/maximized state; drag-to-edge snapping with a
   LibGfx `TileWindowOverlay` preview; a `WindowGeometryOverlay`-style geometry HUD
   (a `ChromeNode` size readout shown while moving/resizing, previewing the tile
   when one is active); and `start_window_resize` routed through the interactive
   resize.
3. **Switcher with live thumbnails (WS3): done.** Alt+Tab (current workspace) /
   Super+Tab (all workspaces), Shift reverses, activate on modifier release; a
   `WindowSwitcher` module listing live `snapshot_toplevel()` thumbnails, title and
   geometry through a `ChromeNode` overlay, most-recently-focused first and
   refreshed on commit while open.
4. **Applet area (WS4): done.** An `AppletManager` module collects the applet
   layer surfaces, orders them by `[Applet] Order` (`Config::applet_order()`) and
   lays them out right-to-left; it emits `applet_area_size_changed` and consumes
   `set_applet_area_position`. The applet's name travels in its layer namespace
   (`serenity-applet:<name>`, set by libgui-wayland), so no protocol extension was
   needed. See the placement-channel decision below.
5. **Notifications (WS5): done.** The unmodified `NotificationServer` is
   spawned on the notify portal and `WindowType::Notification` maps to an overlay
   banner (see the §3 spike). The client path (`LibGUI::Notification`) works once
   a `GUI::Application` event loop exists; `scripts/run-notification-test.sh`
   drives icon/iconless banners, a launch URL and an in-place update, and the
   compositor log asserts the `serenity-notification` overlay layer surface and the
   absence of a managed toplevel.
6. **Tooltips (WS6): done.** Tooltips were inert only because `fast_greet`
   advertised every system effect as disabled; they are now enabled, and the
   `WindowType::Tooltip` window is a `serenity-tooltip` overlay layer surface
   (non-focusable) positioned near the cursor. The compositor's
   `serenity_toplevel.position` reports the content origin so widget-relative
   coordinates are exact. `scripts/run-tooltip-test.sh` drives a pointer over a
   window, requests a tooltip and asserts the `serenity-tooltip` overlay layer
   surface at the cursor offset. Cross-repo, but no protocol extension was needed.

Work-item detail, SerenityOS references and per-item exit tests live in
[`05-r3-plan.md`](05-r3-plan.md) §4.

## 5. Pull forward a minimal safety net (from R4)

R4 owns the session lifecycle, but two pieces are cheap now and turn the result
from "demo" into "usable":

- **Idle, inhibit & lock**: `ext-idle-notify-v1`, `zwp_idle_inhibit_v1`,
  `ext-session-lock-v1`, dim/lock-on-idle and DPMS. A desktop you cannot lock is
  not a daily driver.
- **A supervisor / restart path**, so a compositor fault does not take the
  session with it (N4). Even a minimal "restart the compositor and relaunch the
  shell apps" is worth more than the current all-or-nothing behaviour.

These extend R4's scope but are small compared with the features in §4, and they
protect the work being done now.

## 6. Out of scope for this push

- **R5/R6 breadth**: XWayland, HiDPI/fractional scaling, multi-GPU, portals,
  IME, accessibility, packaging, CI matrix (see
  [`08-compositor-gaps.md`](08-compositor-gaps.md)). Real gaps, but breadth on top
  of a half-finished R3 just spreads effort thin.
- **No new session services or shell polish.** The R2/service-layering work is
  sufficient for now.

## 7. Exit criteria

R3 is done when the [`05-r3-plan.md`](05-r3-plan.md) §1 definition of done holds:
the switcher shows live thumbnails, windows snap, workspaces switch, the applet
area lays out in order, notifications appear as banners, tooltips work, and the
result matches Serenity screenshots structurally. Each scriptable behaviour has a
headless test (§2.4); menus, popups and IME remain manual.

## 8. Risks and decisions

| Risk / decision | Owner step | Note |
|---|---|---|
| Thumbnail capture path | §3 spike | **Resolved**: client-buffer readback; DMA-BUF falls back to the app icon |
| NotificationServer reuse | §3 spike, WS5 | **Resolved**: the unmodified server is reused and the client path is complete; the earlier "IPC encoding" blocker was an event-loop lifetime issue, not an encoding bug |
| Config location/schema | §2.3 | **Resolved**: `WindowServer.ini` + live reload under `$XDG_CONFIG_HOME/sde` |
| `main.cpp` split scope | §2.1 | **Done**: `Overlays`/`WindowManager`/`Model` extracted |
| Applet placement channel | WS4 | Done: the compositor owns the layout (offsets the applet layer surfaces); the applet's identity (its name, for `[Applet] Order`) travels in the layer namespace, so no `serenity-window` extension was needed |
| Tooltips need a host change | WS6 | **Done**: enabled the Tooltips system effect in `libgui-wayland` and mapped `WindowType::Tooltip` to a non-focusable `serenity-tooltip` overlay layer surface; no protocol extension needed |
