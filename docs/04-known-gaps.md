# Serenity Desktop Environment — Known Gaps & Remaining Work

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`05-r3-plan.md`](05-r3-plan.md) · [`07-systemserver-roles.md`](07-systemserver-roles.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md)

This is a running, greppable list of what is known to be missing or incomplete,
so nothing discovered during bring-up is forgotten. It is grouped by goal (G)
and workstream (W) from `01-objectives.md`. Remove entries as work lands; add new
ones as they are found.

Legend: **R?** = milestone; **(host)** = a gap in the `libgui-wayland` port
rather than in this repository.

## R2 — Shell v1 (complete)

Done: Taskbar (system menu, quick-launch, window list, clock), WindowManagerServer
(served by the compositor), wallpaper/desktop background, Desktop
(`FileManager -d`: icons + context menus), file-type icons for known types,
**and the Launch service** (double-click opens the file in the resolved app;
"Open in …" / "Open with …" work).

- **Launch handler coverage is limited (G4 / W4, host).** Handlers come from
  `LaunchServer.ini` `[FileType]`/`[Protocol]` plus the editor as a generic
  alternate. There is no `.af` `AppFile` database, so an app-declared handler set
  (and any real "Open with" chooser beyond the editor) is not available.

## R2 follow-up — shell v1 loose ends

R2 is complete. The `WindowManagerServer` requests the unmodified Taskbar still
calls are now either handled or explicitly reassigned:

- **Window-list context menu (G1 / W2) — done.** Right-clicking a Taskbar
  window-list button calls `popup_window_menu(client_id, window_id, position)`
  (`Userland/Services/Taskbar/TaskbarButton.cpp`). It now opens the compositor's
  window menu for the requested window at the requested position; the menu layout
  clamps it on-screen, so it appears above the Taskbar.
- **Taskbar button geometry for minimize/restore (G1 / W2) — accepted no-op.**
  `set_window_taskbar_rect` is deliberately not consumed: the compositor has no
  minimize/restore animation (`toplevel_toggle_minimized` only toggles scene
  visibility). Revisit if such an animation is added (R3-ish).
- **`start_window_resize` (G3 / W1)** — folded into R3 (snapping/resize; see
  [`05-r3-plan.md`](05-r3-plan.md) §4 WS2).
- **`set_keymap` (G5 / W4)** — belongs with the R3/R4 keymap-switching work, not R2.

The stale, unreachable "right-clicking the titlebar opens the window menu (not
implemented yet)" branch in `WindowServer/src/main.cpp` has been removed (right
click was already handled and returned earlier).

- **Taskbar teardown crash — closed, not reproducible.** A SIGSEGV on Taskbar
  teardown was suspected. It could not be reproduced across compositor loss
  (`WaylandClient` now quits cleanly — libgui-wayland `90af711`), `SIGKILL` of the
  Taskbar, or closing a listed window (stress-tested; all clean). Reopen with a
  reproduction if it is seen again.

## R3 — Serenity look & behaviours (chrome done, rest pending)

- **Menus opened from layer surfaces don't grab input (G3 / W2).** The Taskbar's
  (and Desktop's) menus are overlay layer surfaces; unlike the `xdg_popup` menus
  opened from ordinary windows (which grab), they do not take an input grab, so a
  click on *another* surface does not dismiss them — the menu appears "stuck
  open". It closes only on selecting an item, Escape, or a click on the owner's
  own surface. Needs a compositor-side grab (or an outside-press dismissal) for
  client menus.
- **Window switcher with live thumbnails** — done (WS3). Alt+Tab (current
  workspace) / Super+Tab (all workspaces), Shift reverses, activate on modifier
  release; a Serenity-style list of live thumbnails, title and geometry drawn
  through a `ChromeNode` overlay, most-recently-focused first, refreshed on
  commit while open. The drawing mirrors SerenityOS's `WindowSwitcher`: the
  theme's overlay rect shadow, 64x64 thumbnails, the window title font with a 1px
  drop shadow, the frame rect (`[x,y wxh]`, plus `on row:col` across workspaces)
  right-aligned, mouse hover/click, and the selected window drawn in the theme's
  `Highlighted` window state. In all-workspaces mode the visible workspace follows
  the selection (as SerenityOS switches the window stack while cycling), and any
  key other than Tab/Shift cancels. The window icon is supplied by the client over
  the `serenity-window` protocol (`serenity_toplevel.set_icon`) and shown in the
  titlebar, the Alt+Tab switcher, and the Taskbar window list (forwarded to the
  Taskbar over the WindowManagerServer as `window_icon_bitmap_changed`); clients
  that do not send one show none. The `libgui-wayland` port sends it from
  `GUI::Window::set_icon` via the WindowServer `set_window_icon_bitmap` API.
- **Snapping** — done (WS2). `WindowTileType` + Super+arrows
  (maximize/minimize/tile, Super+Alt horizontal/vertical maximize), drag-to-edge
  snapping with a LibGfx tile preview, `xdg_toplevel` tiled/maximized state, a
  geometry HUD (a `ChromeNode` size readout shown while moving/resizing), and
  `start_window_resize` routed through the interactive resize.
- **Workspaces** — done (WS1). The stack grid comes from `[Workspaces]` in
  `WindowServer.ini`; `set_workspace`/`workspace_changed` and the real `(row,
  column)` in `window_state_changed` drive the Taskbar; Ctrl+Alt+arrows switch
  (Shift carries the focused window); the Super-key WMS events are emitted; and
  the grid size reaches clients in `fast_greet`, so the unmodified
  WorkspacePicker applet renders and switches.
- **Notifications** — done (WS5). The notify portal spawns the **unmodified
  SerenityOS `NotificationServer`** (an `IPC::MultiServer`, like `SQLServer`), and
  the shim maps `WindowType::Notification` to an undecorated **overlay** layer
  surface (`serenity-notification`) at the client's requested position. A
  notification is therefore a banner, top-right and above normal windows, and
  because it is a layer surface rather than a toplevel it is absent from the
  window list and the switcher. The client path is `LibGUI::Notification`;
  `show()` makes a **synchronous** IPC call, so it needs a running
  `Core::EventLoop` (a `GUI::Application`) — every real client (the `notify`
  utility, the Network applet) already provides one. Clicking the banner calls
  `Desktop::Launcher::open` through the launch portal, and hovering expands it;
  this snapshot of the server has no auto-dismiss timer (it closes on click or
  `close_notification`). Covered by `scripts/run-notification-test.sh`.
- **Tooltips** — done (WS6). `GUI::Application::show_tooltip()` was inert because
  the port's `fast_greet` advertised every system effect as disabled; tooltips are
  now enabled, so the Taskbar quick-launch/clock, widgets and applets show hover
  text. A tooltip is a `WindowType::Tooltip` window the client positions near the
  cursor (`get_global_cursor_position()` now reconstructs the cursor from the
  surface under the pointer and its origin) and the compositor creates as an
  undecorated, non-focusable **overlay** layer surface (`serenity-tooltip`), so it
  is above normal windows and out of the window list/switcher. Covered by
  `scripts/run-tooltip-test.sh`.
  - Applet tooltips are now exact: the compositor reports each applet's real
    output position over `serenity-window` (the `position` event now covers
    layer surfaces too, via the applet's chrome handle), so the client's origin
    is no longer stale when the applet area moves it. Window-title button
    tooltips are **not** a gap: SerenityOS's `WindowFrame` sets no button
    tooltips either.
- **Applet area / system tray parity** — done (WS4). The compositor lays applets
  out (AppletManager) right-to-left by `[Applet] Order`, emits
  `applet_area_size_changed` and consumes `set_applet_area_position`, with the
  Taskbar sizing and positioning the area. Applets are identified by the
  `serenity-applet:<name>` layer namespace.
- **Layer-surface configure storm** — fixed. `layer_surface_commit` now
  re-arranges only when the committed state changes the layout (desired size,
  committed size, anchor, margins, exclusive zone, layer), so a content-only
  commit no longer re-configures every layer surface. (Previously
  `arrange_layers` ran on every commit, spinning any client that commits in
  response to a configure.) The committed size is tracked so an applet that
  sizes itself after `show()` (the Audio applet) is re-laid out; see docs/18.
- **Font parity (WS7)** — decided: fixed to the bitmap Katica family, so
  size-based zoom is a no-op. This matches upstream SerenityOS (also Katica) and
  does not block R3; the evidence and the R4/G5 path are in
  [`10-font-parity-decision.md`](10-font-parity-decision.md), with the guard test
  `scripts/run-font-parity-test.sh`.
- **Theme switching (Taskbar ▸ Themes)** — works: the list is read under the resource
  root, the theme is loaded/recoloured in-process, and a shared broker file
  (`$XDG_RUNTIME_DIR/sde-theme`) is watched by every client and the compositor so
  a switch restyles all running apps and the window chrome. As in SerenityOS
  (`keep_desktop_background=false` clears `Background/Color`), a theme switch
  changes the desktop **colour** but does not replace a wallpaper **image** — a
  set wallpaper stays and covers the colour. This is intentional parity, not a
  bug.

## R4 — Session & settings (started)

R4 is broken into milestones **R4.1–R4.9** with sequencing, de-risking spikes and
exit tests in [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md).
Compositor-facing session/input gaps (logind contract, idle/inhibit/lock/DPMS,
keymap/IME, input-device config) are catalogued in
[`08-compositor-gaps.md`](08-compositor-gaps.md) §1 and §4.

**Session integration is portable (R4.3).** logind and systemd are **optional,
runtime-detected, never required**: the session sets the standard environment,
uses `libseat` (`seatd`/`builtin`) for seat/DRM/VT, routes power through a
provider chain, and degrades gracefully with no `xdg-desktop-portal` — see
[`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md).

- **Session safety net (R4.1)** — compositor side landed: `WindowServer` now
  serves `ext-idle-notify-v1`, `zwp_idle_inhibit_v1`, `wlr-output-power-management`
  (DPMS) and `ext-session-lock-v1` (per-output lock surfaces, `locked` once every
  output is covered, input routed to the lock surface on the output under the
  cursor; `Super+L` locks). It is a **standard** `ext-session-lock-v1` server, so
  any conformant locker works — the session uses **swaylock** for now via
  `SDE_LOCK_CMD`. Verified by `scripts/run-idle-test.sh` and
  `scripts/run-lock-test.sh` (including a two-output lock). **Future deliverable:**
  a native SDE lock client (still a plain `ext-session-lock-v1` client, portable to
  other compositors); see [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md)
  §9. Remaining: dim/blank before lock, and re-configuring lock surfaces on output
  resize/scale.
- **Session lifecycle & supervisor (R4.2)** — started: `scripts/sde-session.sh`
  runs XDG Autostart entries, sets the session environment, restarts the
  compositor on a fault (bounded), and arms **idle-to-lock** via the portable
  `sde-idle` daemon (`ext-idle-notify-v1`); `scripts/sde-power.sh` routes
  logout/reboot/shutdown/suspend to logind; `start-sde-session.sh` delegates to
  the supervisor. Verified by `scripts/run-session-test.sh` and the extended
  `scripts/run-idle-test.sh`. The Taskbar `ShutdownDialog` is wired: the
  supervisor installs `/bin` shadow helpers (`shutdown`/`reboot`/`logout`) in
  `SDE_SESSION_BIN_DIR`, which the port's `resolve_executable_path()` honours, so
  the unmodified menu routes to logind (verified by `scripts/run-binpath-test.sh`).
  **Session restore** (`xdg-session-management-v1`, R4.2) landed: the compositor
  serves the standard protocol and persists per-session toplevel geometry under
  `$SDE_STATE_DIR/sessions`, so an app that opts in restores its toplevels across
  a compositor restart — verified by `scripts/run-session-management-test.sh`
  (which restarts the compositor). Remaining: supervision/restart of user apps —
  the supervisor now (re)launches XDG Autostart entries inside its loop once the
  compositor socket is up, so they start after the compositor and are relaunched
  after a restart; supervising long-running apps beyond that is not yet done.

- **Session lifecycle (G6 / W5).** No greet/login, session start, autostart,
  save/restore, lock, logout, reboot, or shut-down; no SystemServer-style
  supervision.
- **Settings & appearance (G5 / W6).** No display (mode/scale/rotation), input,
  appearance or cursor settings backends. Fonts are fixed to the bitmap Katica
  family, so size-based zoom is a no-op. `SDE_WALLPAPER` / `SDE_WALLPAPER_MODE`
  are now only a first-run seed for the wallpaper state (see below).
- **Display Settings app (W6, host).** SerenityOS's `DisplaySettings` builds and
  runs with all tabs (Background, Themes, Fonts, Monitor, Workspaces, Effects);
  launch it from the desktop right-click ▸ "Display Settings" (or
  `scripts/run-sde-app.sh DisplaySettings`). Host seams added: `/res`
  resource-root resolution in `stat`/`opendir` (LibCore), the wallpaper
  directory, and a single-output `get_screen_layout()`. Note: apps must be
  launched into the session with `WAYLAND_DISPLAY=sde`, not the host's display,
  or their windows map on the host compositor — `scripts/run-sde-app.sh` wraps
  that. The Themes tab works (same path as the Taskbar).
- **Wallpaper & desktop background (W6, host).** The Background tab works: the
  WindowServer shim encodes the chosen image and writes the mode and colour to a
  shared state directory (`$XDG_CONFIG_HOME/sde`, override `SDE_STATE_DIR`), and
  the compositor watches that directory and repaints. The choice **persists**
  across restarts: the compositor seeds the state from `SDE_WALLPAPER` /
  `SDE_WALLPAPER_MODE` on first run, after which the state is authoritative (so
  "None" — an empty image — also sticks). `FileManager` additionally exposes
  "Set as wallpaper" on an image. Still inert: Fonts / Monitor / Workspaces /
  Effects (no display, input or font backends yet).
- **ComboBox dropdowns (W6, host).** Resolved — see "Recently resolved".
- **Wallpaper thumbnails (W6/W7, host).** Resolved — see "Recently resolved"
  (`ImageDecoder` is served in-process).
- **Application enablement (W7).** `SystemMonitor` now runs as a **Linux-native**
  app: it reads `/proc` directly (process list, CPU/memory graphs) and unwinds
  thread stacks with **libunwind + elfutils** — no Serenity `/sys/kernel` shim —
  and no longer links the kernel-specific LibSymbolication/LibDebug. Verified by
  `scripts/run-system-monitor-test.sh`. The **Storage** and **Network** tabs now
  read the host too — `/proc/mounts` + `statvfs()` and `getifaddrs()` + sysfs +
  `/proc/net/{tcp,udp,tcp6,udp6}` (`SystemMonitorLinux.cpp`) — instead of
  `/sys/kernel/df` and `/sys/kernel/net/*`. The per-process **Open files** and
  **Memory map** windows read `/proc/<pid>/{fd,fdinfo,smaps}` too; the
  **Unveiled paths** tab is removed on Linux (unveil is a SerenityOS-only
  mechanism). `MouseSettings`, `Debugger`, `CrashReporter` feasibility not yet
  addressed. `Terminal` runs. The **games** (2048, BrickGame, Chess, ColorLines,
  FlappyBug, Flood, GameOfLife, Hearts, MasterWord, Minesweeper, Snake,
  Solitaire, Spider, plus ChessEngine) are built by the port (patch 0035) and so
  appear under the Taskbar's **Games** menu (which it generates from the
  `Base/res/apps/*.af` files); verified by `scripts/run-games-test.sh`. The
  **demos** (CatDog, Eyes, LibGfx Demo, LibGfx Scale Demo, Mandelbrot, Model
  Gallery, Screensaver, Widget Gallery) and **Magnifier** are built too (the
  Demos and Utilities menus; `scripts/run-demos-test.sh`), so the Taskbar's menu
  matches SerenityOS's. `scripts/run-desktop-scene.sh` brings up the desktop
  "scene" (Terminal + File Manager + Firefox + System Monitor on the blue-grid
  wallpaper, headless at the output's size or nested on the host). The
  **DevTools** — GMLPlayground, SQLStudio, HackStudio and **Profiler** — are
  built too and appear under **Development** (`scripts/run-devtools-test.sh`).
  This needed
  **LibDebug**, **LibELF** (core notes), **LibCoredump** and **LibSymbolication**
  to be ported to **native Linux** (patch 0036: Linux ptrace, `struct
  user_regs_struct`, `/proc/<pid>/maps`, except that Linux exposes no readable
  pagemap) rather than a SerenityOS kernel-ABI shim. **`Profiler` runs** on
  native Linux: `LibSerenitySystem::Profiling` (`src/LibSerenitySystem/
  Profiling.{h,cpp}`) samples with `perf_event_open` (`PERF_SAMPLE_IP|TID|TIME|
  CALLCHAIN`, plus mmap/comm/fork/exit records), seeds the current `/proc/<pid>/
  maps` mappings, and writes the same perfcore JSON that `Profiler` parses —
  replacing SerenityOS's `profiling_enable(pid, mask)` + `/proc/<pid>/
  perf_events`. Timestamps are milliseconds, matching `Profile::length_in_ms()`.
  `Profiler` itself is wired to it under `SDE_ROOT` (the Terminal precedent;
  patch 0040) and is built by the port. Verified by `scripts/run-profiler-
  test.sh`. (Sampling *other* users' processes still needs `CAP_PERFMON`; with
  `perf_event_paranoid=2` a user's own processes profile fine because we exclude
  kernel samples.)
- **Web engine / `WebContent` + `RequestServer` + `SQLServer` (W7, host).** Done —
  see "Recently resolved". **Help** (Taskbar system menu ▸ Help) renders the man
  pages; **Browser** loads pages (local `file://` and the WebContent pipeline).

## R5 — Compatibility & ecosystem (not started)

Compositor-facing detail lives in [`08-compositor-gaps.md`](08-compositor-gaps.md) §3–4.

- **XWayland (G7)** — not implemented; native Wayland apps work via `xdg-shell`.
- **Native-app protocol matrix (G7).** `xdg-activation`, `xdg-foreign`,
  `xdg-toplevel-drag`, `data-control`/primary selection, drag-and-drop,
  `keyboard-shortcuts-inhibit`, `relative-pointer`/`pointer-constraints`/
  `pointer-gestures`, `presentation-time`, `fractional-scale`/`wp-viewporter`,
  `content-type`, `tearing-control` are all absent.
- **`xdg-desktop-portal` (G7).** Phase 1 landed: **`xdg-desktop-portal-serenity`**
  (see [`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md)) —
  an SDE backend for the freedesktop portals (Settings, Account, Lockdown,
  Background, AppChooser, Email, Location, ProxyResolver, OpenURI, Notification),
  with a `.portal` file and a `portals.conf` mapping; the real frontend routes to
  it. **Phase 2:** ScreenCast/RemoteDesktop/InputCapture
  (`ext-image-copy-capture-v1` + PipeWire), GlobalShortcuts, DynamicLauncher,
  FileChooser, Print (see [`08-compositor-gaps.md`](08-compositor-gaps.md) §3).
  The compositor/host-blocked remainder (DMA-BUF, metadata cursor, InputCapture,
  Device/Camera) is in
  [`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md) §6:
  the **metadata cursor** mode and **zero-copy DMA-BUF** are landed and verified
  on a live TTY; **Camera** works via the frontend (PipeWire + our Access/Lockdown)
  and `Device` was superseded by USB; **InputCapture** is landed
  ([`17-input-capture.md`](17-input-capture.md)), and a real USB broker remains.
- **Text input / IME (G5) and accessibility (G1).** `text-input-v3` /
  `input-method-v2`, AT-SPI, sticky keys, magnifier and high contrast are absent.

## R6 — Hardening & packaging (not started)

Compositor-facing detail lives in [`08-compositor-gaps.md`](08-compositor-gaps.md) §5–7.

- **HiDPI (integer/fractional) and multi-output** — not done (LibWindowServer M8
  soft-fractional-scaling issue still open — a host dependency, not compositor
  code).
- **Multi-GPU / render-device selection; color management; direct scanout.**
- **Renderer selection & fallback (N2/N5) — done.** `[Output] Renderer`
  (`auto`/`gles2`/`vulkan`/`pixman`, with the `SDE_RENDERER` override) selects the
  wlroots renderer, and a failed GPU path falls back to the software (pixman)
  renderer instead of ending the session; a full software mode is
  `SDE_RENDERER=pixman` + `LIBGL_GPU_DRIVER=softgpu`. Covered by
  `scripts/run-headless-test.sh`.
- **Performance budget & latency instrumentation (N3).** No budget, frame-pacing
  plan or perf-regression test, despite N3 targeting "low predictable input
  latency, 60 Hz".
- **Crash recovery / watchdog / safe mode (N4) — done.** The R4.2 supervisor
  restarts a *crashed* compositor; R6 adds a **watchdog** for a *wedged* one (the
  compositor writes a liveness heartbeat, `SDE_HEARTBEAT_FILE`; the supervisor
  restarts it when the heartbeat goes stale) and **safe mode** (after the restart
  budget, drop to a minimal shell instead of ending the session). User-facing
  session policy is configurable in `$XDG_CONFIG_HOME/sde/session.conf`
  (environment overrides). Covered by `run-session-test.sh` phases (H)–(K) and
  the heartbeat assertion in `run-headless-test.sh`. Full **session restore**
  (relaunching the user's windows after a compositor restart) is not done — it
  needs `xdg-session-management-v1`.
- **Packaging** of `WindowServer` and the session; **headless CI matrix**;
  **sanitizers/fuzzing**; **compositor design doc** and observability.

## R1 — Compositor bring-up (done)

- **Real-hardware leg — exercised.** The DRM/KMS + libinput path
  (`wlr_backend_autocreate` + `scripts/start-sde-session.sh`, with `libseat`
  granting DRM master and handling VT switching) runs on a real TTY, alongside the
  headless (`scripts/run-headless-test.sh`) and nested
  (`scripts/start-sde-nested.sh`) paths. The live-TTY ScreenCast legs were verified
  on this path ([`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md) §6).

## Recently resolved (kept so we don't re-chase them)

- **Shell startup race (SDDM login: applets but no Taskbar/desktop).** The
  compositor spawned its `-s` startup command *before* it started listening on the
  WindowManagerServer socket. The Taskbar, WorkspacePicker and Keymap connect to
  `/tmp/portal/wm` at startup, so on a real login one of them could lose the race
  and abort with `VERIFICATION FAILED` in
  `ConnectionToWindowManagerServer::the()` — leaving a session with only the
  applets that don't use the WM connection. The compositor now calls `listen()` and
  wires the accept handler **before** spawning the shell. Covered by
  `scripts/run-shell-start-test.sh` (runs the session's real startup list).

- **HiDPI & fractional output scaling (R6).** `[Output] DefaultScale` accepts a
  fractional value (0 = auto, 0.5–4.0) and is now applied **live** on a config
  edit (`Config::watch` → `apply_output_scale`); precedence is `SDE_OUTPUT_SCALE` >
  config > saved `screenlayout.ini` > auto. `wp-fractional-scale-v1` is served
  (`wlr_scene` assigns each surface the output's preferred scale), the compositor
  renders its chrome at an integer supersample and uses bilinear filtering for
  non-integer scales, and the client backing store rasters at the output scale
  (host M8 resolved — verified: a Serenity client presents `@2x` on a 2x/1.5x
  output). A saved size with no matching mode requests a **custom mode**, so
  `scripts/run-desktop-scene.sh --headless --scale 2` captures a 2560x1440 HiDPI
  scene. `DisplaySettings`' Monitor tab is **live**: Resolution, Position (X/Y) and
  the **Display scale** control (1x/1.25x/1.5x/2x) apply on **Apply** — the shim
  writes `screenlayout.ini` (mode/position) and `[Output] DefaultScale` (scale,
  as v120 over a new `set_output_scale` IPC; libgui-wayland patch `0042`), and the
  compositor applies both live.

- **Suspend/resume hooks (R4.3/R6).** `scripts/sde-sleep.sh` watches logind's
  `PrepareForSleep` signal (portable: `gdbus` or `dbus-monitor`, a no-op without
  logind/D-Bus). On suspend it locks the session (`SDE_LOCK_CMD`, unless
  `lock_on_suspend=0`); on resume it unblanks (`sde-dpms on`) and runs the optional
  `suspend_hook`/`resume_hook`. The supervisor starts it alongside the idle daemon;
  the hooks come from `session.conf`. Covered by `run-session-test.sh` phase (L).

- **More protocols: single-pixel-buffer, alpha-modifier, xdg-dialog, presentation-time
  (R6).** The compositor now advertises `wp_single_pixel_buffer_manager_v1`,
  `wp_alpha_modifier_v1` and `xdg_wm_dialog_v1` (wlroots implements them; announcing
  them is all that is needed). **`presentation-time`** is served too
  (`wp_presentation v2`): `output_frame` marks the surfaces the frame displays via
  `wlr_presentation_surface_textured_on_output`, so clients receive `presented`
  events. Covered by `run-protocol-matrix-test.sh` (globals) and
  `run-headless-test.sh` (`native-app-test --presentation`).

- **Multi-seat & VT-switch polish (R4.3/R6).** `scripts/sde-seat.sh` derives and
  exports `XDG_SEAT` (default `seat0`) and `XDG_VTNR` (logind, else the active
  VT), and the session namespaces its runtime **per seat**: the Wayland socket
  (`SDE_SESSION_DISPLAY`, `sde` → `sde-<seat>`), the supervisor pid file, the
  `/bin` shadow dir, and the WindowManagerServer socket (`SDE_WM_PORTAL_PATH`,
  `/tmp/portal/wm` → `/tmp/portal/wm-<seat>`; `seat0` keeps the bare names). The
  port relocates the WM portal for clients (libgui-wayland patch `0041`), and the
  compositor logs its seat and gives guidance when the DRM/libinput backend cannot
  start; `start-sde-session.sh` preflights the DRM device. Covered by
  `scripts/run-multiseat-test.sh`.

- **Session watchdog & safe mode (R6/N4).** The compositor writes a liveness
  heartbeat (`SDE_HEARTBEAT_FILE` — a `Core::Timer` touching the file each
  second) and the session supervisor restarts it when the heartbeat goes stale,
  so a *wedged* event loop no longer hangs the session. After the restart budget
  the supervisor drops to **safe mode** (a minimal shell via
  `SDE_SESSION_COMPOSITOR_SAFE`, no autostart) instead of ending the session, and
  only gives up if safe mode also keeps failing. User-facing session policy lives
  in `$XDG_CONFIG_HOME/sde/session.conf` (env overrides the file). Tests:
  `run-session-test.sh` phases (H)–(K), `run-headless-test.sh`.

- **Renderer selection & software fallback (R6/N5).** The compositor honours
  `[Output] Renderer` (`auto`/`gles2`/`vulkan`/`pixman`; `SDE_RENDERER` overrides),
  and when `wlr_renderer_autocreate` fails it retries with the software (pixman)
  renderer rather than returning an error and ending the session. (LibSoftGPU is
  the *client* GL software path, not a wlr_renderer.) Covered by
  `run-headless-test.sh` (a broken DRM render node forces the fallback; an
  explicit `SDE_RENDERER=pixman` selects it directly).

- **Protocol-error isolation (R6/N4).** `tools/protocol-error-test.c` deliberately
  violates a protocol rule (an invalid surface scale; an invalid shm pool size);
  `run-headless-test.sh` checks each error is delivered to the offending client
  (its connection is dropped) while the compositor keeps running and a fresh
  client still maps its window.

- **Crash report & configurable log level (R6/N8).** A fatal signal
  (SIGSEGV/SIGABRT/SIGBUS/SIGILL/SIGFPE) appends the signal name and a backtrace to
  `$SDE_CRASH_LOG` (default `$XDG_CONFIG_HOME/sde/WindowServer-crash.log`) and then
  re-raises, so the crash is diagnosable and the supervisor still restarts the
  session. Verbosity is configurable via `[Log] Level` (`error`/`warn`/`info`/
  `debug`; `SDE_LOG_LEVEL` overrides), which sets wlroots' log level. Covered by
  `run-headless-test.sh` (SIGABRT writes the report; exit 134) and
  `run-config-test.sh` (validation + `SDE_LOG_LEVEL`).

- **Profiler (Development ▸ Profiler).** SerenityOS's kernel profiling interface
  (`profiling_enable(pid, mask)` + `/proc/<pid>/perf_events`) has no Linux
  equivalent, so `LibSerenitySystem::Profiling` samples with `perf_event_open`
  (user callchains, mmap/comm/fork/exit records, `/proc/<pid>/maps` seeding) and
  writes the same perfcore JSON that `Profiler` parses. Wired under `SDE_ROOT`
  (patch 0040); covered by `scripts/run-profiler-test.sh`. Timestamps must be
  **milliseconds** (`Profile::length_in_ms()` is a raw difference used as a
  bucket count) — nanoseconds ballooned RSS to several GiB.

- **Taskbar crash on an invalid window icon.** The compositor sent
  `window_icon_bitmap_changed` with an *invalid* bitmap when a window had no
  icon, and `TaskbarWindow::wm_event` dereferences it (SerenityOS windows always
  have at least the default icon). Every toplevel now gets the default window
  icon (`window.png`) at creation (the client's `set_icon` overrides it, and
  clearing resets to the default), so the message is always valid.

- **Scrollback in the Terminal (mouse wheel / trackpad).** Wayland axis deltas
  are frequently fractional (trackpads, high-resolution wheels); the port
  truncated each one with `wl_fixed_to_int`, so sub-1 deltas became 0 and
  nothing scrolled (libgui-wayland, `WaylandClient`). Now accumulated into whole
  steps; covered by `scripts/run-scroll-test.sh`.
- **TUI scrolling (opencode, vim, tmux).** Full-screen apps use the alternate
  screen (no scrollback by design) and enable mouse reporting so they scroll
  themselves; LibVT ignored `DECSET 1000/1002/1003/1006`, so they received no
  wheel events. LibVT now implements mouse reporting and forwards SGR/legacy
  events (libgui-wayland patch `0032`); covered by
  `scripts/run-terminal-mouse-test.sh`.

- **Calculator (and apps registering launch handlers) crashed at startup.** A
  *stale* `/tmp/session/<sid>/portal/launch` socket made the port "defer to the
  session service", which was gone; `Desktop::Launcher`'s `connection()` then
  `VERIFY`d and the app aborted. The port now **probes** the portal socket and, if
  nothing is listening, unlinks it and serves the launch portal **in-process**
  (`src/LibWM.cpp`); a live session `LaunchServer` is still preferred. Calculator
  runs and renders correctly at both 1x and 2x.

- **Apps launched by the session LaunchServer opened on the wrong compositor.** When nested, the session `LaunchServer` was started with the *host* `WAYLAND_DISPLAY`, so the applications it spawns (e.g. the desktop's "Display Settings") connected to the host (their window was created at the `-5000,-5000` sentinel) instead of joining the SDE session. Session services that spawn apps now run with the **session** display (`scripts/start-sde-nested.sh`, `scripts/start-sde-session.sh`); the compositor already sets `WAYLAND_DISPLAY` for its own children.
- **The session supervisor restarted on SIGTERM** instead of stopping, so the session could not be shut down (e.g. logout). It now treats SIGTERM/SIGINT as "stop": it kills the compositor and exits the loop. Covered by `scripts/run-session-test.sh` phase (F).
- **ComboBox dropdown** — regression test added (`scripts/run-combo-test.sh`, `libwm-combo-test`): opening a ComboBox popup (the Display Settings wallpaper "Mode" box) must produce a real popup, not a collapsed sliver.
- **Desktop ▸ "Display Settings" now opens DisplaySettings.** `Desktop::Launcher::open("/bin/DisplaySettings")` (the desktop right-click entry) resolved the URL by file extension — landing on the wildcard handler (`/bin/TextEditor`) — and `stat`ed the host path `/bin/DisplaySettings`, which does not exist, so nothing opened. The port's `LaunchServerConnection::open_url` now runs an **executable regular file** directly (mirroring the upstream `LaunchServer`), mapping Serenity's `/bin/X` to the session app dir like the rest of the port. The desktop entry now launches the same DisplaySettings dialog the Settings app spawns. Verified by `scripts/run-launch-exec-test.sh`.
- Compositor: window destroyed before decoration (protocol error); frozen
  "ghost" window (scene tree not torn down); lost mouse-up (implicit pointer
  grab); F11 fullscreen no-op; window menu dismissed on its opening click;
  menus could not open from layer-surface windows (Desktop/Taskbar/applets);
  menu anchors off-screen for layer windows.
- Compositor: window placement is now reported to clients
  (`serenity_toplevel.position`), so an app's screen-relative coordinates match
  where the compositor actually put the window. This fixed **ComboBox dropdowns**
  (and tooltips/popups): they used to collapse to a 2px-tall popup because the
  app clamped an off-screen rect against the desktop. Client menus opened from
  layer surfaces are now also dismissed by an outside press
  (`serenity_window_manager.dismiss_menus`), instead of appearing stuck open.
- Display Settings: **Apply no longer freezes.** Wallpapers are now stored as raw
  pixels (PNG-encoding cost ~800 ms on the UI thread); the compositor watches the
  right file and coalesces the burst of state events into one repaint; the Monitor
  tab's apply is skipped (output configuration is unsupported) instead of raising
  the modal "keep this screen layout? reverting in N seconds" dialog; and several
  synchronous WindowServer calls the tabs make (`set_system_fonts`,
  `apply_workspace_settings`, `get_system_theme_override`, `get_screen_layout`)
  now return valid responses instead of blocking the application forever.
- Port (host): empty-size window `VERIFY` (Desktop); first layer configure not
  delivered, so the Desktop widget tree never laid out; `Process::spawn`
  working-directory `TODO()` crash; LaunchServer stub returned invalid
  responses (broke the file context menu); `FileIconProvider` couldn't find its
  patterns (`/etc` absent from the sparse checkout); Serenity `/bin/X` helper
  paths not resolved to host binaries (launching failed with ENOENT); Launch
  service implemented (handler resolution + spawn).
- **`ImageDecoder` portal** — served in-process by libgui-wayland (its in-process
  fallback) and, when SDE runs, by the `ImageDecoder/` session service. This fixes
  the `FileSystemModel`/wallpaper thumbnails (`connect: No such file or
  directory`) and lets **ImageViewer** decode images. The in-process host decodes
  **inline on its own thread**: `ImageDecoder`'s upstream job runs on
  `Threading::BackgroundAction`, the single process-global thread that
  `FileSystemModel`'s thumbnail renderer already occupies, so sharing it deadlocked
  thumbnails — a separate process does not. See `06-service-layering.md` §4.
- **Out-of-process web engine** — `WebContent` + `RequestServer` are built by
  `libgui-wayland` and spawned on demand by `LibWM` (a socketpair handed over via
  `SOCKET_TAKEOVER`); `SQLServer` is hosted with a listening socket handed over
  (see `07-systemserver-roles.md` §2.3). **Help** renders the SerenityOS man pages
  (relocated from `Base/usr/share/man`); **Browser** loads pages, including
  `https://` — HTTPS verifies against the host trust store (LibTLS patch `0024`).
  The session seeds the skeleton user config (`scripts/seed-serenity-home.sh`) that
  `Browser` reads at startup. Known limit: LibTLS cannot parse ECDSA roots
  (`id-ecPublicKey`), so it skips them; RSA roots load and verify fine.
- Documented: bitmap-font zoom limitation (`02-compositor-decision.md` §11).
