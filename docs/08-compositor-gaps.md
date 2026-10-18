# Serenity Desktop Environment: Compositor Gaps

**Status:** Living document · **Date:** 2026-10-18
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

- **logind/`libseat` contract (G6): done for the logind path.** VT switching, DRM
  master handover and device access stay with `libseat`/`wlr_backend_autocreate`
  (by design). SDE owns the seat identity and failure story: it derives/exports
  `XDG_SEAT`/`XDG_VTNR`, namespaces its runtime per seat (Wayland socket, pid file,
  `/bin` shadow dir, `SDE_WM_PORTAL_PATH`), and fails with actionable guidance when
  there is no seat/DRM device (`scripts/run-multiseat-test.sh`). Suspend/resume
  hooks are in `scripts/sde-sleep.sh` (lock on suspend, re-apply DPMS on resume).
  Still open: real-hardware verification of the portable `seatd` and `builtin`
  seat backends.
- **Idle / inhibit / lock / DPMS (G6): done.** `ext-idle-notify-v1`,
  `zwp_idle_inhibit_v1`, `ext-session-lock-v1` (per-output lock surfaces, input
  routed to the lock, re-configure on output size/scale changes) and DPMS via
  `sde-dpms` are served. `Super+L` uses any conformant locker; a native SDE lock
  client is still a future deliverable, not a protocol gap.
- **System-session integration (G6): done (portable).** The standard session
  environment (`XDG_SESSION_TYPE=wayland`, `WAYLAND_DISPLAY`,
  `XDG_CURRENT_DESKTOP`, `dbus-update-activation-environment` where a bus exists)
  is set by the supervisor; logind/systemd are optional, with no
  `graphical-session.target` dependency
  ([`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md)).

## 2. Robustness, recovery & configuration (R6, plus R3/R4 for config)

- **Crash recovery / watchdog / safe mode (N4): done.** The R4.2 supervisor
  restarts a crashed compositor, a heartbeat watchdog restarts a wedged one, and
  repeated failures drop to safe mode instead of ending the session. Full session
  restore (`xdg-session-management-v1` plus the compositor's live app list) is
  covered by `run-session-test.sh`/`run-session-management-test.sh`. The shell and
  long-running services have their own supervisors (`sde-shell.sh`,
  `sde-service-supervisor.sh`).
- **Safe mode / renderer fallback (N2, N5): done.** `[Output] Renderer`
  (`auto`/`gles2`/`vulkan`/`pixman`; `SDE_RENDERER` overrides) selects the
  renderer; a failed GPU path falls back to the software renderer instead of
  ending the session. Covered by `run-headless-test.sh`.
- **Protocol-error & state-machine coverage (N4, N8): partial.** Error
  *isolation* is covered (`tools/protocol-error-test.c` plus
  `run-headless-test.sh`), and structured fuzzers cover config, the
  WindowManagerServer and the xdg-shell surface/toplevel path. Broader
  conformance of configure/commit sequences, decoration negotiation, popup/layer
  grabs, buffer release, dmabuf import failure and `wl_resource` lifetimes is
  still thin.
- **Compositor configuration system (G1, G3): done.** `WindowServer.ini`,
  `input.ini` and `screenlayout.ini` are validated (`ConfigRules`), watched and
  live-reloaded; display/input/appearance/workspace/effect settings drive them.
  Per-frame tracing/sampling and a formal policy schema for every WM rule remain
  future work.

## 3. Ecosystem compatibility (R5)

The compatibility bar is **KDE Plasma / KWin's standard-protocol surface**; SDE
runs the native-app protocol matrix tests and the X11 suite, but there is no
GTK/Qt/Firefox conformance matrix in CI yet.

- **No compatibility target or conformance suite (G7, N8).** The protocol matrix
  (`run-protocol-matrix-test.sh`) and apps-smoke/X11 tests exercise many clients,
  but a GTK/Qt/SDL/Firefox reference run is still manual.
- **Focus/attention (G3, G7): done.** `xdg-activation-v1` is served; launching
  from the Taskbar uses it.
- **Sandbox & portal interop (G7): mostly done.** `xdg-foreign` v1/v2 and
  `xdg-wm-dialog-v1` are served; `xdg-system-bell-v1` and
  `xdg-toplevel-icon-v1` are served. `xdg-toplevel-drag` is not present in the
  pinned wlroots.
- **Clipboard completeness (G4): done.** `set_selection`, primary selection
  (`zwp_primary_selection_device_manager_v1`) and both data-control variants are
  served. Explicit data-device drag-and-drop is routed by the compositor with a
  drag icon, and the port bridges LibGUI's `DragOperation` both ways.
- **Input protocols (G3, G7): done for the implemented set.**
  `keyboard-shortcuts-inhibit`, `relative-pointer`, `pointer-constraints`
  (lock/confine), `pointer-gestures`, touch, switch devices and
  `zwp-tablet-v2`/`zwp-tablet-pad-v2` are served. The headless harness injects
  touch/tablet through the test-only `serenity-virtual-input` protocol.
- **Presentation & scaling (N3, N5): largely done.** `wp-viewporter`,
  `content-type-v1`, `tearing-control`, `wp-single-pixel-buffer`,
  `wp-alpha-modifier`, `presentation-time` and `wp-fractional-scale-v1` are
  served; output scale is fractional and live. Still missing: `commit-timing` /
  `fifo` (wlroots bump) and dmabuf feedback/explicit sync
  (`wp-linux-drm-syncobj`).
- **Portals (G7): done.** `xdg-desktop-portal-serenity` implements the
  freedesktop portal interfaces; ScreenCast/RemoteDesktop/InputCapture,
  GlobalShortcuts, DynamicLauncher, FileChooser and Print are in place, along
  with the USB broker and Access/Lockdown. See
  [`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md) and
  [`17-input-capture.md`](17-input-capture.md). The remaining portal work is
  host/GPU-gated (dmabuf, metadata cursor) and documented there.
- **XWayland (G7): first-class window manager.** Rootless XWayland, scaling,
  clipboard/primary selection, `_NET_WM_ICON`/`WM_CLASS`, override-redirect,
  `_NET_WM_STATE`/transients, `_NET_WORKAREA` and Taskbar grouping are handled
  ([`20-xwayland-next-steps.md`](20-xwayland-next-steps.md)). Xwayland keyboard
  grabs are not in the pinned wlroots; XDND needs 0.21.

## 4. Input & accessibility (R4, R5)

- **Text input / IME (G5): done (R5).** `text-input-v3` and `input-method-v2`
  are served; the compositor relays preedit/commit, keyboard grabs and candidate
  popups. See [`22-input-method.md`](22-input-method.md),
  `scripts/run-ime-test.sh`.
- **Virtual input (N8, G5): done as a test/OSK seam.** The compositor serves
  `wlr-virtual-keyboard`, `wlr-virtual-pointer` and the SDE-only
  `serenity-virtual-input` touch/tablet protocol. The scripted tests use them;
  they also underpin an on-screen keyboard.
- **Touch, pen and tablet (G7): done.** Touch is routed through the cursor and
  `wlr_seat`; pen/tablet and tablet-pad v2 are served; the headless harness
  injects both. Covered by `run-virtual-touch-test.sh`.
- **Keyboard management (G5): partial.** `[Keyboard]` layouts/variant/model/
  options are applied at startup and live; `set_keymap` (the WM-server request)
  is still a stub, and per-device/per-window layout plus compose/dead-key
  handling are unplanned.
- **Input device configuration (G5): done for the settings SDE exposes.**
  Acceleration, acceleration profile, scroll step, natural scroll,
  tap-to-click, left-handed and buttons-switched are configured and persisted
  (`input.ini`, Mouse/Keyboard Settings). Disable-while-typing and per-device
  profiles are not exposed.
- **Accessibility (G1): done (R5).** The compositor provides sticky keys and high
  contrast; the Magnifier captures the committed frame; and LibGUI publishes its
  widget tree over AT-SPI (`LibAccessibility`, with `Accessible`, `Component`,
  `Action`, `Text`, `EditableText` and state/text events). See
  [`29-accessibility.md`](29-accessibility.md) and
  [`30-at-spi.md`](30-at-spi.md); the sticky-keys, high-contrast, magnifier and
  AT-SPI tests cover them.

## 5. Output & rendering depth (R6)

- **Multi-GPU / render-device selection (N5).** No `wlr_multi_backend` or
  DRI_PRIME-style render-device choice; the compositor uses one renderer.
- **Direct scanout / overlay planes / VRR (N3).** `wlr_scene_output_commit` runs
  with no direct-scanout options; adaptive sync and per-monitor VRR are not
  exposed. The `tearing-control` protocol is served.
- **Color management / HDR (N5).** No `wlr-color-management`, gamma or color
  profiles.
- **Capture & leasing (G3).** General screencopy/`ext-image-copy-capture` is
  served (screenshots, ScreenCast, the Magnifier's region capture); `wp-drm-lease`
  (VR) is not planned.
- **Performance budget & instrumentation (N3): done.** Frame time,
  input → present latency and memory are metered; `scripts/run-perf-test.sh`
  enforces the budget headless (including "no frames while idle") and
  `run-session-memory-test.sh` the PSS budgets. See
  [`25-performance.md`](25-performance.md). A GL 240 Hz zero-slow-frame run and
  input-to-photon on a real DRM output remain hardware checks.
- **Multi-output / hotplug (N5): done for the output model.** Per-output
  resolution/position/scale apply live in DisplaySettings; disabling/re-enabling
  an output moves windows, re-fits maximized windows and restores the configured
  main screen. Covered by `run-output-hotplug-test.sh`. Multi-GPU remains.

## 6. Architecture, observability & docs (R6)

- **Compositor design document: done.** [`31-compositor-design.md`](31-compositor-design.md)
  records the single-threaded event loop, ownership/listener/reverse-teardown
  rules, scene/render/capture model, focus and the lock, IPC/service boundaries,
  configuration, failure paths and a checklist for new code.
- **wlroots version policy & protocol versioning.** The pin is 0.20.2 and the
  upgrade policy is in [`21-r5-protocol-parity-plan.md`](21-r5-protocol-parity-plan.md);
  `serenity-window` still has no formal versioning/extension policy.
- **Observability (N8): done except unthrottled per-frame tracing.** `[Log] Level`/
  `SDE_LOG_LEVEL`, the fatal-signal crash report and an opt-in JSONL event trace
  (`SDE_TRACE_FILE`, `run-trace-test.sh`) are in place. `SDE_TRACE_FRAMES=1` adds
  sampled frame events (at most one per second) with frame time and budget; a
  full per-frame stream remains open by design.
- **WM policy details (G3): partial.** Focus-follows-mouse vs click is
  configurable (`[Focus] FollowsMouse`), transient/modal stacking and session
  restore of placement are implemented. Focus-stealing prevention,
  cross-output maximize and struts beyond layer-shell remain unspecified.

## 7. Testing & CI breadth (R6)

- **Sanitizers and fuzzing (N8): sanitizers and five fuzz targets done.**
  ASan/UBSan builds and the full-tier sanitizer run found and fixed a shutdown
  use-after-free; config, WindowManagerServer and xdg-shell fuzzers are in place
  (the WM one found the huge-Taskbar-rect abort). Two more harnesses drive the
  layer-shell arrange path (`run-layer-fuzz-test.sh`) and data-device
  source/selection/drag ownership (`run-data-device-fuzz-test.sh`). A full-tier
  Valgrind teardown profile (`run-valgrind-test.sh`) covers start, map and clean
  shutdown with no definite leaks; it found and fixed the client-chrome and
  repeating-timer shutdown leaks.
- **Pixel/golden tests across scale and output count (N8).** The R3 harness has
  scale/output captures; extend it to the full scale matrix, multiple outputs and
  the software/GPU renderer split.
- **Accessibility & IME testing (G5): done headless** (`run-atspi-test.sh`,
  `run-ime-test.sh`, sticky/high-contrast/magnifier tests).
- **Real hardware** (DRM/KMS, multi-GPU, color, direct scanout) still needs a
  machine, not a runner.

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
