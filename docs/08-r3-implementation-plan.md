# Serenity Desktop Environment — R3 Implementation Plan

**Status:** Draft v0.1 · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`04-known-gaps.md`](04-known-gaps.md) · [`05-r3-plan.md`](05-r3-plan.md) · [`06-compositor-gaps.md`](06-compositor-gaps.md) · [`07-systemserver-roles.md`](07-systemserver-roles.md)

[`05-r3-plan.md`](05-r3-plan.md) defines **what** R3 is: the work items, the
SerenityOS reference code, and an exit test for each. This document is its
execution companion — **in what order and how** to build it, what to de-risk
first, and what to deliberately leave alone. Keep the two in sync: 05 owns scope,
this document owns sequencing and the critical path.

---

## 1. Where R3 stands

- R0, R1 and R2 are done; R2's loose ends are closed or explicitly reassigned
  (see `04-known-gaps.md`, "R2 follow-up").
- R3 is **half-landed**: SSD Serenity window chrome, the window menu, fullscreen,
  Show Desktop, theme-broker restyle and wallpaper already work. The *behaviours*
  — switcher, snapping, workspaces, applet area, notifications, tooltips — are not
  started.
- R3 is the only milestone that both defines the product "feel" and is already
  under way, so it is the critical path.

**Strategy:** finish R3 before taking on R4+ breadth. The service scaffolding
(`LaunchServer`, `ImageDecoder`, the layering/SystemServer-role work) is now
sufficient; stop adding services or shell polish until the behaviours land.

## 2. Foundation before features

Every remaining R3 feature lands in a policy area that SerenityOS keeps as its
own class (`WindowManager`, `WindowSwitcher`, `Overlays`, `AppletManager`). Build
that structure first — doing four features inside one 2,800-line `main.cpp` is
where the schedule goes. This is §3 of [`05-r3-plan.md`](05-r3-plan.md), promoted
here to the first task.

1. **Module split.** Extract `Overlays`, then `WindowManager`,
   `WindowSwitcher`, `AppletManager`, leaving `main.cpp` as wiring. Keep wlroots
   behind the C++ platform layer (D5).
2. **Overlay + snapshot infrastructure.** A persistent LibGfx-drawn chrome node in
   `overlay_tree`, and a way to snapshot a toplevel's scene subtree into a
   `wlr_buffer` for thumbnails. Shared by snapping (preview) and the switcher.
3. **Compositor config — pull forward from R4.** Workspaces grid, snapping
   behaviour, keybindings and applet order all need a config schema and live
   reload, and no milestone currently owns one. Extend the existing
   `$XDG_CONFIG_HOME/sde` state directory (already used for wallpaper/theme)
   rather than inventing a second mechanism. Retrofitting config after the
   features exist is far more expensive.
4. **Scriptable tests.** Extend the headless harness with virtual keyboard/pointer
   so R3 behaviours are testable from the first feature, not bolted on later.

**First commit:** extract `Overlays` plus the snapshot helper. It is the smallest
piece that both de-risks the switcher and forces the module split in a controlled
way.

## 3. De-risk before committing (time-boxed spikes)

Two unknowns decide how the features are built. Resolve them before the features
that depend on them, each with a clear exit criterion and a fallback.

| Spike | Question | Exit / fallback |
|---|---|---|
| **Thumbnail capture** | Composite the toplevel scene subtree into a buffer, or use `ext-image-copy-capture`? | A cached per-toplevel thumbnail that renders in the switcher; fallback to icon + stale frame. Decides WS3's implementation. |
| **NotificationServer reuse** | Does the unmodified SerenityOS `NotificationServer` run as a session client under `WindowType::Notification`, or do we need a shim + banner client? | A banner appears with correct window-type semantics; fallback to the service-shim pattern. |

Both are listed as risks in [`05-r3-plan.md`](05-r3-plan.md) §7; this document
schedules them as prerequisites rather than late surprises.

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

1. **Workspaces (WS1)** — the keystone. Unblocks the switcher's "all windows"
   mode and makes the already-running WorkspacePicker applet live. Includes
   `set_workspace`, `workspace_changed`, the Super-key WMS events and the window
   stack grid.
2. **Snapping + overlays (WS2)** — shares the overlay infrastructure.
3. **Switcher with live thumbnails (WS3)** — depends on 1 and 2.
4. **Applet area (WS4)** — independent; needs the applet placement-channel
   decision first.
5. **Notifications (WS5)** — independent; gated on the §3 spike.
6. **Tooltips (WS6)** — cross-repo (a host change in `libgui-wayland`);
   independent, can run in parallel.

Work-item detail, SerenityOS references and per-item exit tests live in
[`05-r3-plan.md`](05-r3-plan.md) §4.

## 5. Pull forward a minimal safety net (from R4)

R4 owns the session lifecycle, but two pieces are cheap now and turn the result
from "demo" into "usable":

- **Idle, inhibit & lock** — `ext-idle-notify-v1`, `zwp_idle_inhibit_v1`,
  `ext-session-lock-v1`, dim/lock-on-idle and DPMS. A desktop you cannot lock is
  not a daily driver.
- **A supervisor / restart path** — so a compositor fault does not take the
  session with it (N4). Even a minimal "restart the compositor and relaunch the
  shell apps" is worth more than the current all-or-nothing behaviour.

These extend R4's scope but are small compared with the features in §4, and they
protect the work being done now.

## 6. Out of scope for this push

- **R5/R6 breadth** — XWayland, HiDPI/fractional scaling, multi-GPU, portals,
  IME, accessibility, packaging, CI matrix (see
  [`06-compositor-gaps.md`](06-compositor-gaps.md)). Real gaps, but breadth on top
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
| Thumbnail capture path | §3 spike | Determines WS3; fallback to icon + stale frame |
| NotificationServer reuse | §3 spike | Fallback: shim + banner client |
| Applet placement channel | WS4 | Compositor offsets vs a `serenity-window` extension |
| Tooltips need a host change | WS6 | In `libgui-wayland`; cross-repo coordination |
| Config location/schema | §2.3 | Extend `$XDG_CONFIG_HOME/sde`; define keys before features need them |
| `main.cpp` split scope | §2.1 | Time-box; keep behaviour identical while extracting |
