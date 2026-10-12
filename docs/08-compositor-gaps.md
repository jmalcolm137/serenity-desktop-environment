# Serenity Desktop Environment — Compositor Gaps

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`04-known-gaps.md`](04-known-gaps.md) · [`05-r3-plan.md`](05-r3-plan.md)

`04-known-gaps.md` lists what is missing from the milestones **as written**.
This document is the complement: what is missing from the **plan for
`WindowServer` as a general-purpose Wayland compositor**. The project is
Serenity-first but commits to running native Wayland apps well (`01-objectives.md`
G7), so these gaps are not optional polish — they decide whether the result is
trustworthy enough to daily-drive.

Entries are grouped by theme and tagged with the goal (`G`) and the milestone
(`R`) that should own them. Legend: **G?** = goal, **R?** = milestone,
**(host)** = a gap in `libgui-wayland` rather than this repository.

Context for the current state: `WindowServer/src/main.cpp` `initialize()`
registers `wl_compositor`, `wl_subcompositor`, `wl_data_device_manager`,
`wl_output`/`xdg_output_v1`, the scene, `xdg_shell`, `wlr-layer-shell`,
`xdg-decoration`, `wlr-foreign-toplevel-management`, `wlr-output-management`,
the cursor and the seat. Everything below that is *not* in that list is absent
until proven otherwise.

---

## 1. Session, seat & power (R4)

R4 says "lock/power" but owns no mechanism. These belong with the session work,
not R5/R6.

- **logind/`libseat` contract (G6).** VT switching, DRM master handover,
  device access, suspend/resume, lid, and multi-seat are left to
  `wlr_backend_autocreate`; the plan does not say what SDE owns or how it fails.
- **Idle / inhibit / lock / DPMS (G6).** No `ext-idle-notify-v1`,
  `zwp_idle_inhibit_v1`, `ext-session-lock-v1`, dim/lock-on-idle, or
  DPMS/backlight handling. `ext-session-lock-v1` in particular must blank and
  lock every output, including hotplugged ones, which is non-trivial.
- **System-session integration (G6).** No description of the standard session
  environment (`XDG_SESSION_TYPE=wayland`, `WAYLAND_DISPLAY` propagation,
  `dbus-update-activation-environment` where a bus exists) that apps and portals
  rely on. Resolved as **portable** — logind/systemd optional, no
  `graphical-session.target` dependency
  ([`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md)).

## 2. Robustness, recovery & configuration (R6, plus R3/R4 for config)

- **Crash recovery / watchdog (N4).** A compositor crash kills every client and
  loses the session. No plan for a supervisor, restart, session restore, or
  preserving window/app state across a restart.
- **Safe mode / renderer fallback (N2, N5).** No selection logic or fallback
  session for when the GPU path fails (`wlr_renderer_autocreate` is used
  directly); no documented "start with Pixman" path for recovery.
- **Protocol-error & state-machine coverage (N4, N8).** xdg configure/commit,
  the initial commit, decoration negotiation, popup/layer grabs, buffer release,
  dmabuf import failure and `wl_resource` lifetimes have no conformance or fuzz
  tests. One protocol error can currently take the whole display down.
- **Compositor configuration system (G1, G3).** No schema for keybindings, focus
  policy, tile/snap behaviour, workspace grid, animations, or output defaults,
  and no live reload. SerenityOS reads `WindowServer.ini`; SDE has no equivalent
  — and the R3 workspaces/snapping/keybinding work needs one. The settings
  "backend seam" only covers write-paths to appearance/display, not the
  compositor's own policy.

## 3. Ecosystem compatibility (R5)

The compatibility bar: **parity with KDE Plasma / KWin's standard protocol
surface** (the broadest and most current), tracked as a matrix in
[`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §11. The gaps
below are what remains to reach it; `02-compositor-decision.md` §13 lists the
protocols, nothing today tests them per-compositor except the community support
table.

- **No compatibility target or conformance suite (G7, N8).** Add "N reference
  clients run correctly" as an exit criterion — `weston-simple-*`, a GTK and a Qt
  app, SDL/games, Firefox — exercised in CI.
- **Focus/attention (G3, G7).** `xdg-activation-v1` (raise/attention tokens) is
  served; launching from the Taskbar uses it.
- **Sandbox & portal interop (G7).** `xdg-foreign` v1/v2 (toplevel import, used by
  portals and XWayland) are served; `xdg-toplevel-drag` and `xdg-system-bell`
  remain.
- **Clipboard completeness (G4).** `set_selection` and **primary selection**
  (`zwp_primary_selection_device_manager_v1`) are served; missing
  `wlr-data-control`/`ext-data-control` and full **drag-and-drop** (gestures, drag
  icon, enter/leave choreography).
- **Input protocols (G3, G7).** `keyboard-shortcuts-inhibit` and
  `relative-pointer` are served; `pointer-constraints` and `pointer-gestures`
  remain — needed for games and remote desktop.
- **Presentation & scaling (N3, N5).** `wp-viewporter`, `content-type-v1` and
  `tearing-control` are served. Still missing: `presentation-time`,
  `fractional-scale-v1` (needs the preferred-scale plumbing), `wp-single-pixel-buffer`,
  `commit-timing`/`fifo`, and dmabuf feedback/explicit sync.
- **Portals (G7).** No `xdg-desktop-portal` strategy. The project's *Serenity*
  portals (`/tmp/session/<id>/portal/…`) are unrelated to the freedesktop
  `xdg-desktop-portal`. Needed: a backend selection for a wlroots compositor
  (`xdg-desktop-portal-wlr` for Screenshot/ScreenCast, `-gtk` as fallback) plus a
  `portals.conf` mapping; an SDE **Settings** portal backend (`color-scheme`,
  `accent-color`, `contrast`) for R4; and, for R5, RemoteDesktop/InputCapture/
  GlobalShortcuts (lean on `ext-image-copy-capture-v1` + PipeWire). Note the theme
  broker will eventually need to interoperate with the Settings portal's
  `color-scheme`. The SDE backend is now implemented
  ([`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md)); its
  TTY-only work (DMA-BUF capture, metadata cursor, InputCapture, Device/Camera)
  is enumerated in that doc's §6 — zero-copy DMA-BUF and the metadata cursor are
  landed and verified on a live TTY; InputCapture is landed (compositor-hosted
  EIS + the D-Bus backend, [`17-input-capture.md`](17-input-capture.md)); Device
  was superseded by USB and Camera works via the frontend.
- **XWayland (G7).** A single R5 bullet today; rootless XWayland, scaling,
  clipboard/DnD integration and rootful fallback is a large feature that needs
  its own plan.

## 4. Input & accessibility (R4, R5)

- **Text input / IME (G5).** No `text-input-v3` or `input-method-v2`; the docs
  only mention IME as manual verification. This blocks CJK input and on-screen
  keyboards on a desktop.
- **Virtual input (N8, G5).** `wlr-virtual-keyboard`/`wlr-virtual-pointer` are
  listed in the protocol appendix but owned by no milestone — yet the R3 scripted
  tests depend on them, and they underpin an on-screen keyboard.
- **Touch & pen (G7).** No touch or `wlr-tablet` support.
- **Keyboard management (G5).** `set_keymap` is a stub; multiple layouts,
  per-device/per-window layout, and compose/dead-key handling are unplanned.
- **Input device configuration (G5).** R4 says "input settings" but does not
  enumerate libinput: accel profile/speed, tap-to-click, natural scroll, button
  mapping, disable-while-typing, left-handed.
- **Accessibility (G1).** AT-SPI/screen reader, sticky keys, magnifier and
  high-contrast are entirely absent. For a "classic workstation" aesthetic this
  is a notable hole.

## 5. Output & rendering depth (R6)

- **Multi-GPU / render-device selection (N5).** No plan for outputs on a
  secondary GPU, `wlr_multi_backend`, or DRI_PRIME-style render-device choice.
- **Direct scanout / overlay planes / VRR / tearing (N3).** `wlr_scene_output_commit`
  runs with no options today; no direct scanout, adaptive sync, or tearing
  control.
- **Color management / HDR (N5).** No `wlr-color-management`, gamma or color
  profiles.
- **Capture & leasing (G3).** Only the R3 switcher needs capture; there is no
  general screencopy/`ext-image-copy-capture` or `wp-drm-lease` (VR) plan.
- **Performance budget & instrumentation (N3).** N3 sets "low predictable input
  latency, smooth 60 Hz" but there is no latency budget, frame-pacing plan, or
  measurement — and no perf-regression test.
- **HiDPI is cross-repo (N5, host).** R6 lists it, but the real blocker is the
  LibWindowServer client backing-store scale (host M8), not compositor code; the
  dependency must be stated explicitly. An **integer output-scale stopgap** now
  makes high-DPI panels usable (nearest-filtered 2x; see
  [`13-native-session-and-aur-packaging.md`](13-native-session-and-aur-packaging.md)
  Appendix A); the backing-store scale remains the real fix.

## 6. Architecture, observability & docs (R6)

- **Compositor design document.** The event-loop model is novel and undocumented:
  Wayland events are pumped through a `Core::Notifier` into `Core::EventLoop`
  (see `serenity_main`), not `wl_display_run`. Written invariants (reentrancy,
  timer ownership, threading, render model) are needed before more features land.
- **wlroots version policy & protocol versioning.** wlroots API drift is an
  accepted risk with no upgrade cadence; `serenity-window` has no versioning or
  extension policy.
- **Observability (N8).** `wlr_log_init(WLR_ERROR, …)` is fixed; no configurable
  log levels, tracing, or crash-dump policy.
- **WM policy details (G3).** Focus-follows-mouse vs click, focus-stealing
  prevention, transient/modal dialogs, cross-output maximize, struts beyond
  layer-shell, and session restore of window placement are unspecified.

## 7. Testing & CI breadth (R6)

- **Sanitizers & fuzzing (N8).** No ASan/UBSan/Valgrind in CI, and no fuzzing of
  the compositor's protocol entry points.
- **Pixel/golden tests across scale and output count (N8).** The R3 harness
  starts pixel assertions; extend to fractional scale, multiple outputs, and
  software vs GPU renderer.
- **Accessibility & IME testing (G5).** Not addressed.

---

## Milestone ownership summary

| Theme | Owner | Notes |
|---|---|---|
| Session/seat/power, idle/lock/DPMS | **R4** | extends the existing "lock/power" scope |
| Compositor config + live reload | **R3/R4** | needed by the R3 workspaces/snapping/keybindings |
| Crash recovery / safe mode | **R6** | satisfies N4 |
| Ecosystem protocols, portals, XWayland | **R5** | satisfies G7 |
| IME, virtual input, keymap, input config | **R4** | satisfies G5 |
| Accessibility | **R5** | grouped with ecosystem completeness |
| Output/render depth, perf, multi-GPU, HiDPI | **R6** | satisfies N3/N5 |
| Architecture docs, observability | **R6** | |
| Testing & CI breadth | **R6** | satisfies N8 |

R5 and R6 exist because the original single "R5 — Hardening & packaging"
milestone bundled two independent bodies of work (ecosystem compatibility and
hardening/packaging) that would otherwise compete for the same effort and let
the compatibility work stall. See [`01-objectives.md`](01-objectives.md) §11.
