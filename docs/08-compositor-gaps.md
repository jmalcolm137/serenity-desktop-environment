# Serenity Desktop Environment: Compositor Gaps

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`04-known-gaps.md`](04-known-gaps.md) · [`05-r3-plan.md`](05-r3-plan.md)

`04-known-gaps.md` lists what is missing from the milestones **as written**.
This document is the complement: what is missing from the **plan for
`WindowServer` as a general-purpose Wayland compositor**. The project is
Serenity-first but commits to running native Wayland apps well (`01-objectives.md`
G7), so these gaps are not optional polish; they decide whether the result is
trustworthy enough to daily-drive.

Entries are grouped by theme and tagged with the goal (`G`) and the milestone
(`R`) that should own them. Legend: **G?** = goal, **R?** = milestone,
**(host)** = a gap in `libgui-wayland` rather than this repository.

Context for the current state: `WindowServer/src/main.cpp` `initialize()` (and the
subsystem `initialize_*()` functions it calls) registers `wl_compositor`, `wl_subcompositor`, `wl_data_device_manager`,
`wl_output`/`xdg_output_v1`, the scene, `xdg_shell`, `wlr-layer-shell`,
`xdg-decoration`, `wlr-foreign-toplevel-management`, `wlr-output-management`,
the cursor and the seat. Everything below that is *not* in that list is absent
until proven otherwise.

---

## 1. Session, seat & power (R4)

R4 says "lock/power" but owns no mechanism. These belong with the session work,
not R5/R6.

- **logind/`libseat` contract (G6): mostly resolved.** VT switching, DRM master
  handover and device access stay with `libseat`/`wlr_backend_autocreate` (by
  design), but SDE now owns the seat *identity* and the failure story: the
  session derives/exports `XDG_SEAT`/`XDG_VTNR`, namespaces its runtime per seat
  (Wayland socket, pid file, `/bin` shadow dir, `SDE_WM_PORTAL_PATH`), and fails
  with actionable guidance when there is no seat/DRM device
  (`scripts/run-multiseat-test.sh`). Still open: suspend/resume hooks and lid.
- **Idle / inhibit / lock / DPMS (G6).** No `ext-idle-notify-v1`,
  `zwp_idle_inhibit_v1`, `ext-session-lock-v1`, dim/lock-on-idle, or
  DPMS/backlight handling. `ext-session-lock-v1` in particular must blank and
  lock every output, including hotplugged ones, which is non-trivial.
- **System-session integration (G6).** No description of the standard session
  environment (`XDG_SESSION_TYPE=wayland`, `WAYLAND_DISPLAY` propagation,
  `dbus-update-activation-environment` where a bus exists) that apps and portals
  rely on. Resolved as **portable**: logind/systemd optional, no
  `graphical-session.target` dependency
  ([`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md)).

## 2. Robustness, recovery & configuration (R6, plus R3/R4 for config)

- **Crash recovery / watchdog (N4).** A compositor crash kills every client and
  loses the session. No plan for a supervisor, restart, session restore, or
  preserving window/app state across a restart.
- **Safe mode / renderer fallback (N2, N5): done.** `[Output] Renderer`
  (`auto`/`gles2`/`vulkan`/`pixman`; the `SDE_RENDERER` environment variable
  overrides it) selects the wlroots renderer, and if the GPU path fails the
  compositor falls back to the software (pixman) renderer instead of ending the
  session (the R4.2 supervisor's safe mode covers a bad shell). See
  [`04-known-gaps.md`](04-known-gaps.md) R6.
- **Protocol-error & state-machine coverage (N4, N8).** Error *isolation* is now
  covered: `tools/protocol-error-test.c` deliberately violates a protocol rule (an
  invalid surface scale, an invalid shm pool size) and `run-headless-test.sh`
  asserts the error is delivered to that client, the compositor keeps serving, and
  a fresh client still maps. Broader conformance/fuzz coverage of xdg
  configure/commit, the initial commit, decoration negotiation, popup/layer grabs,
  buffer release, dmabuf import failure and `wl_resource` lifetimes is still
  missing.
- **Compositor configuration system (G1, G3).** No schema for keybindings, focus
  policy, tile/snap behaviour, workspace grid, animations, or output defaults,
  and no live reload. SerenityOS reads `WindowServer.ini`; SDE has no equivalent,
  and the R3 workspaces/snapping/keybinding work needs one. The settings
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
  clients run correctly" as an exit criterion (`weston-simple-*`, a GTK and a Qt
  app, SDL/games, Firefox), exercised in CI.
- **Focus/attention (G3, G7).** `xdg-activation-v1` (raise/attention tokens) is
  served; launching from the Taskbar uses it.
- **Sandbox & portal interop (G7).** `xdg-foreign` v1/v2 (toplevel import, used by
  portals and XWayland) and `xdg-wm-dialog-v1` are served; `xdg-toplevel-drag`
  (needs a wlroots bump) and `xdg-system-bell` remain.
- **Clipboard completeness (G4).** `set_selection`, **primary selection**
  (`zwp_primary_selection_device_manager_v1`) and both **data-control** variants
  (`zwlr_data_control_manager_v1`, `ext_data_control_manager_v1`, for clipboard
  managers such as the ClipboardHistory applet) are served. **Explicit
  drag-and-drop** (data-device DnD between windows) is served too: the compositor
  accepts a client's drag, draws its icon above every layer, and routes
  enter/motion/drop to the surface under the cursor. The port bridges LibGUI's
  `DragOperation` onto it (source and target), so Serenity apps interoperate with
  standard Wayland clients. Remaining: touch-drag polish and pointer-gestures.
- **Input protocols (G3, G7).** `keyboard-shortcuts-inhibit`, `relative-pointer`,
  `pointer-constraints` (lock/confine, enforced in the cursor-motion path) and
  `pointer-gestures` (touchpad swipe/pinch/hold) are served. **Touch** input is
  routed (cursor touch events → `wlr_seat` touch notifications; the drag icon
  follows a touch point), and **switch** devices (lid/tablet-mode/keypad-slide)
  are tracked. **Pen/tablet** is served as `zwp-tablet-v2` (proximity, tip,
  motion, pressure/tilt/rotation/slider/wheel, tool buttons) and **tablet-pad**
  as `zwp-tablet-pad-v2` (express buttons, ring and strip). Because wlroots has
  no virtual touch/tablet and the headless backend has no libinput, the harness
  injects them through an SDE test-only protocol (`serenity-virtual-input`,
  `scripts/run-virtual-touch-test.sh`), which also drives the touch-drag path
  (the drag icon follows the touch point).
- **Presentation & scaling (N3, N5): largely done.** `wp-viewporter`,
  `content-type-v1`, `tearing-control`, `wp-single-pixel-buffer`,
  `wp-alpha-modifier`, **`presentation-time`** (`wp_presentation v2`; the
  compositor feeds wlroots' presentation object from `output_frame`) and
  **`wp-fractional-scale-v1`** are served. The output scale may be fractional
  (`[Output] DefaultScale`, applied live), the compositor renders its chrome at an
  integer supersample, and the client backing store rasters at the output scale
  (host M8 resolved); see [`13`](13-native-session-and-aur-packaging.md) Appendix
  A. Still missing: `commit-timing`/`fifo` (needs a wlroots bump) and dmabuf
  feedback/explicit sync (`wp-linux-drm-syncobj`).
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
  is enumerated in that doc's §6: zero-copy DMA-BUF and the metadata cursor are
  landed and verified on a live TTY; InputCapture is landed (compositor-hosted
  EIS + the D-Bus backend, [`17-input-capture.md`](17-input-capture.md)); Device
  was superseded by USB and Camera works via the frontend.
- **XWayland (G7).** A single R5 bullet today; rootless XWayland, scaling,
  clipboard/DnD integration and rootful fallback is a large feature that needs
  its own plan.

## 4. Input & accessibility (R4, R5)

- **Text input / IME (G5): done (R5).** `text-input-v3` (clients) and
  `input-method-v2` (input methods) are served: the compositor relays the focused
  field's state to the input method and applies its preedit/commit back, routes
  the keyboard to the input method's grab, and renders candidate popups. See
  [`22-input-method.md`](22-input-method.md); `scripts/run-ime-test.sh`. This
  unblocks CJK input and on-screen keyboards (a real input method client is a
  manual check).
- **Virtual input (N8, G5).** `wlr-virtual-keyboard`/`wlr-virtual-pointer` are
  listed in the protocol appendix but owned by no milestone, yet the R3 scripted
  tests depend on them, and they underpin an on-screen keyboard.
- **Touch & pen (G7).** No touch or `wlr-tablet` support.
- **Keyboard management (G5).** `set_keymap` is a stub; multiple layouts,
  per-device/per-window layout, and compose/dead-key handling are unplanned.
- **Input device configuration (G5).** R4 says "input settings" but does not
  enumerate libinput: accel profile/speed, tap-to-click, natural scroll, button
  mapping, disable-while-typing, left-handed.
- **Accessibility (G1): done (R5).** The compositor provides sticky keys and high
  contrast; the Magnifier captures the committed frame; and LibGUI publishes its
  widget tree over AT-SPI (`LibAccessibility`, with `Accessible`, `Component`,
  `Action`, `Text`, `EditableText` and state/text events). See
  [`29-accessibility.md`](29-accessibility.md) and
  [`30-at-spi.md`](30-at-spi.md); `scripts/run-sticky-keys-test.sh`,
  `run-high-contrast-test.sh`, `run-magnifier-test.sh` and
  `run-atspi-test.sh` cover them.

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
- **Performance budget & instrumentation (N3): done.** The compositor meters
  its own frame time and the input → present interval; the budget, the method and
  the reference numbers are in [`25-performance.md`](25-performance.md), and
  `scripts/run-perf-test.sh` enforces it headless on the software renderer
  (including "no frames while idle").
- **Multi-output / hotplug (N5): coarse.** The compositor handles several
  outputs and a saved layout, but per-output mode/scale and hotplug behaviour is
  basic. **HiDPI** itself is done: fractional output scale, the client backing
  store and host M8 all landed (see the scaling section above and
  [`13-native-session-and-aur-packaging.md`](13-native-session-and-aur-packaging.md)).

## 6. Architecture, observability & docs (R6)

- **Compositor design document.** The event-loop model is novel and undocumented:
  Wayland events are pumped through a `Core::Notifier` into `Core::EventLoop`
  (see `serenity_main`), not `wl_display_run`. Written invariants (reentrancy,
  timer ownership, threading, render model) are needed before more features land.
- **wlroots version policy & protocol versioning.** wlroots API drift is an
  accepted risk with no upgrade cadence; `serenity-window` has no versioning or
  extension policy.
- **Observability (N8).** Verbosity is configurable (`[Log] Level` =
  `error`/`warn`/`info`/`debug`, overridden by `SDE_LOG_LEVEL`); it sets wlroots'
  log level and gates our own messages. A fatal-signal **crash report** (the signal
  name plus a backtrace) is written to `$SDE_CRASH_LOG` (default the state dir's
  `WindowServer-crash.log`) before the signal is re-raised, so a crash is
  diagnosable and the supervisor still restarts the session (R6). Still missing:
  tracing and structured logs.
- **WM policy details (G3).** Focus-follows-mouse vs click, focus-stealing
  prevention, transient/modal dialogs, cross-output maximize, struts beyond
  layer-shell, and session restore of window placement are unspecified.

## 7. Testing & CI breadth (R6)

- **Sanitizers & fuzzing (N8).** No ASan/UBSan/Valgrind in CI, and no fuzzing of
  the compositor's protocol entry points.
- **Pixel/golden tests across scale and output count (N8).** The R3 harness
  starts pixel assertions; extend to fractional scale, multiple outputs, and
  software vs GPU renderer.
- **Accessibility & IME testing (G5): done.** `run-atspi-test.sh` walks a live
  window's AT-SPI tree over a private D-Bus (roles, extents, actions, text,
  editable text and change events), and `run-ime-test.sh` exercises the
  text-input/input-method relay headless. Sticky keys, high contrast and the
  Magnifier have their own headless tests.

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

R5 and R6 exist because the original single "R5: Hardening & packaging"
milestone bundled two independent bodies of work (ecosystem compatibility and
hardening/packaging) that would otherwise compete for the same effort and let
the compatibility work stall. See [`01-objectives.md`](01-objectives.md) §11.
