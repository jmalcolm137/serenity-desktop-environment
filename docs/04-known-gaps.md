# Serenity Desktop Environment: Known Gaps & Remaining Work

**Status:** Living document · **Date:** 2026-10-16
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`05-r3-plan.md`](05-r3-plan.md) · [`07-systemserver-roles.md`](07-systemserver-roles.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md)

This is a running, greppable list of what is known to be missing or incomplete,
so nothing discovered during bring-up is forgotten. It is grouped by goal (G)
and workstream (W) from `01-objectives.md`. Remove entries as work lands; add new
ones as they are found.

Legend: **R?** = milestone; **(host)** = a gap in the `libgui-wayland` port
rather than in this repository.

## R2: Shell v1 (complete)

Done: Taskbar (system menu, quick-launch, window list, clock), WindowManagerServer
(served by the compositor), wallpaper/desktop background, Desktop
(`FileManager -d`: icons + context menus), file-type icons for known types,
**and the Launch service** (double-click opens the file in the resolved app;
"Open in …" / "Open with …" work).

- **Launch handler coverage is limited (G4 / W4, host).** Handlers come from
  `LaunchServer.ini` `[FileType]`/`[Protocol]` plus the editor as a generic
  alternate. There is no `.af` `AppFile` database, so an app-declared handler set
  (and any real "Open with" chooser beyond the editor) is not available.

## R2 follow-up: shell v1 loose ends

R2 is complete. The `WindowManagerServer` requests the unmodified Taskbar still
calls are now either handled or explicitly reassigned:

- **Window-list context menu (G1 / W2): done.** Right-clicking a Taskbar
  window-list button calls `popup_window_menu(client_id, window_id, position)`
  (`Userland/Services/Taskbar/TaskbarButton.cpp`). It now opens the compositor's
  window menu for the requested window at the requested position; the menu layout
  clamps it on-screen, so it appears above the Taskbar.
- **Taskbar button geometry for minimize/restore (G1 / W2): done.** The Taskbar
  reports each window button's rect (`set_window_taskbar_rect`); the compositor
  stores it and animates the minimize/restore outline between the window and that
  button (SerenityOS `Window::start_minimize_animation`). See "Window animations"
  in the R4 "Recently resolved" list.
- **`start_window_resize` (G3 / W1)**: folded into R3 (snapping/resize; see
  [`05-r3-plan.md`](05-r3-plan.md) §4 WS2).
- **`set_keymap` (G5 / W4)**: belongs with the R3/R4 keymap-switching work, not R2.

The stale, unreachable "right-clicking the titlebar opens the window menu (not
implemented yet)" branch in `WindowServer/src/main.cpp` has been removed (right
click was already handled and returned earlier).

- **Taskbar teardown crash: closed, not reproducible.** A SIGSEGV on Taskbar
  teardown was suspected. It could not be reproduced across compositor loss
  (`WaylandClient` now quits cleanly; libgui-wayland `90af711`), `SIGKILL` of the
  Taskbar, or closing a listed window (stress-tested; all clean). Reopen with a
  reproduction if it is seen again.

## R3: Serenity look & behaviours (done)

- **Window switcher with live thumbnails**: done (WS3). Alt+Tab (current
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
- **Snapping**: done (WS2). `WindowTileType` + Super+arrows
  (maximize/minimize/tile, Super+Alt horizontal/vertical maximize), drag-to-edge
  snapping with a LibGfx tile preview, `xdg_toplevel` tiled/maximized state, a
  geometry HUD (a `ChromeNode` size readout shown while moving/resizing), and
  `start_window_resize` routed through the interactive resize. Snapping only arms
  when the pointer is close to the edge (`[Tiling] EdgeThreshold`, default 16px)
  and is disabled entirely by `[Tiling] DragToEdge=false`; covered by
  `scripts/run-edge-snap-test.sh`.
- **Workspaces**: done (WS1). The stack grid comes from `[Workspaces]` in
  `WindowServer.ini`; `set_workspace`/`workspace_changed` and the real `(row,
  column)` in `window_state_changed` drive the Taskbar; Ctrl+Alt+arrows switch
  (Shift carries the focused window); the Super-key WMS events are emitted; and
  the grid size reaches clients in `fast_greet`, so the unmodified
  WorkspacePicker applet renders and switches.
- **Notifications**: done (WS5). The notify portal spawns the **unmodified
  SerenityOS `NotificationServer`** (an `IPC::MultiServer`, like `SQLServer`), and
  the shim maps `WindowType::Notification` to an undecorated **overlay** layer
  surface (`serenity-notification`) at the client's requested position. A
  notification is therefore a banner, top-right and above normal windows, and
  because it is a layer surface rather than a toplevel it is absent from the
  window list and the switcher. The client path is `LibGUI::Notification`;
  `show()` makes a **synchronous** IPC call, so it needs a running
  `Core::EventLoop` (a `GUI::Application`); every real client (the `notify`
  utility, the Network applet) already provides one. Clicking the banner calls
  `Desktop::Launcher::open` through the launch portal, and hovering expands it;
  this snapshot of the server has no auto-dismiss timer (it closes on click or
  `close_notification`). Covered by `scripts/run-notification-test.sh`.
- **Tooltips**: done (WS6). `GUI::Application::show_tooltip()` was inert because
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
- **Applet area / system tray parity**: done (WS4). The compositor lays applets
  out (AppletManager) right-to-left by `[Applet] Order`, emits
  `applet_area_size_changed` and consumes `set_applet_area_position`, with the
  Taskbar sizing and positioning the area. Applets are identified by the
  `serenity-applet:<name>` layer namespace.
- **Layer-surface configure storm**: fixed. `layer_surface_commit` now
  re-arranges only when the committed state changes the layout (desired size,
  committed size, anchor, margins, exclusive zone, layer), so a content-only
  commit no longer re-configures every layer surface. (Previously
  `arrange_layers` ran on every commit, spinning any client that commits in
  response to a configure.) The committed size is tracked so an applet that
  sizes itself after `show()` (the Audio applet) is re-laid out; see docs/18.
- **Font parity (WS7)**: decided: fixed to the bitmap Katica family, so
  size-based zoom is a no-op. This matches upstream SerenityOS (also Katica) and
  does not block R3; the evidence and the R4/G5 path are in
  [`10-font-parity-decision.md`](10-font-parity-decision.md), with the guard test
  `scripts/run-font-parity-test.sh`.
- **Theme switching (Taskbar ▸ Themes)**: works: the list is read under the resource
  root, the theme is loaded/recoloured in-process, and a shared broker file
  (`$XDG_RUNTIME_DIR/sde-theme`) is watched by every client and the compositor so
  a switch restyles all running apps and the window chrome. As in SerenityOS
  (`keep_desktop_background=false` clears `Background/Color`), a theme switch
  changes the desktop **colour** but does not replace a wallpaper **image**; a
  set wallpaper stays and covers the colour. This is intentional parity, not a
  bug.

### SerenityOS window-manager behaviours

The window chrome and the interactions are done: titlebar buttons, the window
menu (minimize / maximize / restore / move / roll-up / menu bar / always on top /
close), the icon-click menu, double-click-titlebar maximize, move and resize by
the frame, **`Super`+left-drag to move / `Super`+right-drag to resize from
anywhere**, **unmaximize-on-move**, **`Alt+Shift` keyboard-layout switching**,
**minimize/restore/launch animations and the window-stack transition**, the
window-stack-switch overlay, `Super+H` (cursor highlight),
`Ctrl+Super+Shift+I` (reload icon bitmaps), snapping, tiling (`Super`+arrows,
`Super+Alt` horizontal / vertical maximize), the Alt+Tab / Super+Tab switcher
with live thumbnails, the workspace grid (with carry), fragile-window dismissal
and the `Super`-key events forwarded to the WindowManagerServer. What follows are
the behaviours from SerenityOS's
`Userland/Services/WindowServer/{WindowManager,Window,Compositor}.cpp` that SDE
still does **not** reproduce in full.

- **Modal and transient (parent/child) windows**: **done.** The client's
  SerenityOS `WindowMode` reaches the compositor over `serenity-window`
  (`serenity_toplevel.set_window_mode`), and its `xdg_toplevel.parent` marks the
  transient parent. A **blocking** modal preempts its modal chain: input behind
  it is blocked (the hit-test skips a blocked window, so a click does not focus
  it), the modal cannot be minimized, and a transient stays above its parent
  (SerenityOS `Window::blocking_modal_window()`). The blocked state is reported
  to the Taskbar. Covered by `scripts/run-modal-test.sh`.
  (Not yet: cross-client modal chains (an X11 modal blocking an xdg window and
  vice versa) and SerenityOS's explicit "lower the blockers" pass.)
- **Automatic cursor tracking windows.** SerenityOS's
  `set_automatic_cursor_tracking_window` keeps delivering pointer events to the
  window that received a press until every button is released (and lets several
  windows track at once). SDE has the single-window form as its **implicit
  pointer grab** (`Server::pointer.grab_surface` in `WindowServer/src/Pointer.cpp`,
  set on a press and cleared when the last button is released, mirroring
  SerenityOS's `process_ongoing_active_input_mouse_event`); SerenityOS's
  multi-window tracking and its `did_popup_a_menu` reset are not reproduced.

Not gaps, for the record: SDE's drag-to-edge snapping is an *addition* over stock
SerenityOS (which tiles only through `Super`+arrows), and the minimal
`serenity-window` chrome is a deliberate P8 choice, not a missing behaviour.

## R4: Session & settings (substantially done)

R4 is broken into milestones **R4.1–R4.9** with sequencing, de-risking spikes and
exit tests in [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md).
Compositor-facing session/input gaps (logind contract, idle/inhibit/lock/DPMS,
keymap/IME, input-device config) are catalogued in
[`08-compositor-gaps.md`](08-compositor-gaps.md) §1 and §4.

**Session integration is portable (R4.3).** logind and systemd are **optional,
runtime-detected, never required**: the session sets the standard environment,
uses `libseat` (`seatd`/`builtin`) for seat/DRM/VT, routes power through a
provider chain, and degrades gracefully with no `xdg-desktop-portal`; see
[`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md).

- **Session safety net (R4.1)**: compositor side landed: `WindowServer` now
  serves `ext-idle-notify-v1`, `zwp_idle_inhibit_v1`, `wlr-output-power-management`
  (DPMS) and `ext-session-lock-v1` (per-output lock surfaces, `locked` once every
  output is covered, input routed to the lock surface on the output under the
  cursor; `Super+L` locks). It is a **standard** `ext-session-lock-v1` server, so
  any conformant locker works; the session uses **swaylock** for now via
  `SDE_LOCK_CMD`. Verified by `scripts/run-idle-test.sh` and
  `scripts/run-lock-test.sh` (including a two-output lock). **Future deliverable:**
  a native SDE lock client (still a plain `ext-session-lock-v1` client, portable to
  other compositors); see [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md)
  §9. Remaining: dim/blank before lock, and re-configuring lock surfaces on output
  resize/scale.
- **Session lifecycle & supervisor (R4.2)**: started: `scripts/sde-session.sh`
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
  a compositor restart, verified by `scripts/run-session-management-test.sh`
  (which restarts the compositor). **Full session restore now completes**: the
  compositor records the applications that have a mapped window
  (`$SDE_STATE_DIR/session-apps`, rewritten as windows come and go), and the
  supervisor relaunches them once the compositor socket is up: on a restart, and
  on the next login. Apps that opted into `xdg-session-management-v1` then restore
  their toplevels' geometry; an app the user closed is not restored, one that is
  no longer installed is skipped, and the apps the session's own startup command
  launches are not duplicated. The supervisor still (re)launches XDG Autostart
  entries the same way; supervising long-running apps beyond that is not done.

- **Session lifecycle (G6 / W5): resolved (R4.1–R4.9).** Greet/login stays the
  display manager's; the session now starts, autostarts, locks, idles, saves/
  restores toplevels (`xdg-session-management-v1`) and powers off/reboots, with
  logind optional. See [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md).
- **Settings & appearance (G5 / W6): resolved (R4.4–R4.7).** Display (mode/scale/
  arrangement), input (libinput/keymaps), appearance/effects and cursor settings
  backends are in, driven by a live-reloadable config and exposed via the Settings
  portal; the session **font broker** now supports a scalable default, so
  size-based zoom works. `SDE_WALLPAPER`/`SDE_WALLPAPER_MODE` remain only the
  first-run seed for the wallpaper state.
- **Display Settings app (W6, host).** SerenityOS's `DisplaySettings` builds and
  runs with all tabs (Background, Themes, Fonts, Monitor, Workspaces, Effects);
  launch it from the desktop right-click ▸ "Display Settings" (or
  `scripts/run-sde-app.sh DisplaySettings`). Host seams added: `/res`
  resource-root resolution in `stat`/`opendir` (LibCore), the wallpaper
  directory, and a single-output `get_screen_layout()`. Note: apps must be
  launched into the session with `WAYLAND_DISPLAY=sde`, not the host's display,
  or their windows map on the host compositor; `scripts/run-sde-app.sh` wraps
  that. The Themes tab works (same path as the Taskbar).
- **Wallpaper & desktop background (W6, host).** The Background tab works: the
  WindowServer shim encodes the chosen image and writes the mode and colour to a
  shared state directory (`$XDG_CONFIG_HOME/sde`, override `SDE_STATE_DIR`), and
  the compositor watches that directory and repaints. The choice **persists**
  across restarts: the compositor seeds the state from `SDE_WALLPAPER` /
  `SDE_WALLPAPER_MODE` on first run, after which the state is authoritative (so
  "None" (an empty image) also sticks). `FileManager` additionally exposes
  "Set as wallpaper" on an image. Still inert: Fonts / Monitor / Workspaces /
  Effects (no display, input or font backends yet).
- **ComboBox dropdowns (W6, host).** Resolved; see "Recently resolved".
- **Wallpaper thumbnails (W6/W7, host).** Resolved; see "Recently resolved"
  (`ImageDecoder` is served in-process).
- **Application enablement (W7).** `SystemMonitor` now runs as a **Linux-native**
  app: it reads `/proc` directly (process list, CPU/memory graphs) and unwinds
  thread stacks with **libunwind + elfutils** (no Serenity `/sys/kernel` shim)
  and no longer links the kernel-specific LibSymbolication/LibDebug. Verified by
  `scripts/run-system-monitor-test.sh`. The **Storage** and **Network** tabs now
  read the host too, `/proc/mounts` + `statvfs()` and `getifaddrs()` + sysfs +
  `/proc/net/{tcp,udp,tcp6,udp6}` (`SystemMonitorLinux.cpp`), instead of
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
  **DevTools** (GMLPlayground, SQLStudio, HackStudio and **Profiler**) are
  built too and appear under **Development** (`scripts/run-devtools-test.sh`).
  This needed
  **LibDebug**, **LibELF** (core notes), **LibCoredump** and **LibSymbolication**
  to be ported to **native Linux** (patch 0036: Linux ptrace, `struct
  user_regs_struct`, `/proc/<pid>/maps`, except that Linux exposes no readable
  pagemap) rather than a SerenityOS kernel-ABI shim. **`Profiler` runs** on
  native Linux: `LibSerenitySystem::Profiling` (`src/LibSerenitySystem/
  Profiling.{h,cpp}`) samples with `perf_event_open` (`PERF_SAMPLE_IP|TID|TIME|
  CALLCHAIN`, plus mmap/comm/fork/exit records), seeds the current `/proc/<pid>/
  maps` mappings, and writes the same perfcore JSON that `Profiler` parses,
  replacing SerenityOS's `profiling_enable(pid, mask)` + `/proc/<pid>/
  perf_events`. Timestamps are milliseconds, matching `Profile::length_in_ms()`.
  `Profiler` itself is wired to it under `SDE_ROOT` (the Terminal precedent;
  SDE patch 0004-profiler-libserenitysystem) and is built by the port. Verified by `scripts/run-profiler-
  test.sh`. (Sampling *other* users' processes still needs `CAP_PERFMON`; with
  `perf_event_paranoid=2` a user's own processes profile fine because we exclude
  kernel samples.)
  The **Debugger** is now Linux-native too: it drives `ptrace` directly, with
  `PTRACE_GETREGSET`/`NT_PRSTATUS` for the register set (some host kernels reject
  `PTRACE_GETREGS` with `EFAULT`), `__WALL` for `waitpid`, and Linux single-step
  semantics. It is built by the port (patches 0050/0052) and verified by
  `scripts/run-debugger-test.sh` (breakpoint, registers, backtrace, disassembly
  against a real host process). The **CrashReporter** GUI is built and reads a
  native Linux ELF core dump: `LibELF` tolerates a section-table-less ELF and
  OS-specific program headers, and `LibCoredump` has a Linux backend over the
  standard notes (`NT_PRSTATUS`/`NT_PRPSINFO`/`NT_FILE`/`NT_SIGINFO`/`NT_AUXV`)
  and the `PT_LOAD` segments (patch 0053; `scripts/run-crashreporter-test.sh`).
  **Not yet wired:** *how* a crash reaches `CrashReporter`. On Linux the kernel
  routes crashes through `kernel.core_pattern` (here
  `|/usr/lib/systemd/systemd-coredump ...`), so nothing launches `CrashReporter`
  automatically. The candidate designs, to be chosen later, are (a) an SDE
  `core_pattern` pipe handler (`|…/sde-coredump`) that saves the core to a
  per-user drop dir, the closest analogue to SerenityOS's kernel→CrashReporter
  path, but a system-wide, privileged install; (b) a session watcher that reads
  new cores via `coredumpctl` (works without privileges, but depends on systemd,
  which R4.3 keeps optional); or (c) a session watcher over the drop dir, with
  (a) feeding it so the root-side handler never has to reach into the user's
  Wayland session. Until one is implemented, `CrashReporter <core>` reads a core
  explicitly.
- **Web engine / `WebContent` + `RequestServer` + `SQLServer` (W7, host).** Done;
  see "Recently resolved". **Help** (Taskbar system menu ▸ Help) renders the man
  pages; **Browser** loads pages (local `file://` and the WebContent pipeline).

## R5: Compatibility & ecosystem (substantially done)

Compositor-facing detail lives in [`08-compositor-gaps.md`](08-compositor-gaps.md) §3–4.

- **XWayland (G7)**: **landed, working toward KWin/Plasma parity.** The compositor
  runs Xwayland lazily; X11 clients map, focus, stack (content, position,
  click-to-focus, close), and get **server-side decoration frames** (unless the
  client asks for no decorations via Motif hints): titlebar drag-to-move, border
  resize, and close/maximize/minimize buttons, plus Super+drag move/resize.
  X11 windows also join the **Taskbar window list** (reported to the
  WindowManagerServer by id, so the Taskbar can activate/minimize them).
  `[Compositor] XWayland=false` in `WindowServer.ini` disables X11 entirely (no X
  server, no `DISPLAY`). Verified by `scripts/run-xwayland-test.sh`.
  **Landing since:** X11 windows joined the **Alt-Tab** switcher (the `WindowRef`
  abstraction, `07027fc`) and the **workspace grid**; they carry their
  **`_NET_WM_ICON`**/**`WM_HINTS`** icons and **WM_CLASS** (`b3ac13a`,
  `a95fa67`); the window is placed with its frame at the client's requested
  position and **kept on-screen** (titlebar visible), with a distinct X11
  **default icon** when the client supplies none; override-redirect
  menus/tooltips are polished (`cb21165`); the X11 `PRIMARY`/`CLIPBOARD`
  **selection bridge** works (`6c8e0c9`); `_NET_WM_STATE`
  (`above`/`skip-taskbar`/`demands-attention`/`modal`) and **transients** are
  honoured (`c69b1ae`); and the **workarea** (`_NET_WORKAREA`) is reported
  (`47f794f`). An exiting X11 client no longer aborts the compositor (the destroy
  handler detaches its `wlr_xwayland_surface` listeners, `2d557e2`), and iconless
  X11 clients report the default "X" icon to the Taskbar/switcher. X11 windows
  carry their **`WM_CLASS`** to the Taskbar, which can **group** a button per
  application when `[Taskbar] GroupWindows` is on (default off; SerenityOS's
  Taskbar has one button per window; see `docs/14`). See
  [`20-xwayland-next-steps.md`](20-xwayland-next-steps.md).
  **A dead X server can no longer take the compositor down:** `update_xwayland_workareas()`
  now checks `wlr_xwayland_get_xwm_connection()` before reporting the workarea.
  wlroots drops the XWM connection when the X server goes away, and the previous
  code called into a dead Xwayland from a layer-surface commit, a SIGSEGV that
  killed a live session (found by the CI work, [`26-ci.md`](26-ci.md) §6.2).
  X11 grouping is verified by `run-taskbar-grouping-test.sh` (`a1e3cb9`): an X11
  client's `WM_CLASS` can arrive after it maps (the first state reports
  `class=''`), and the Taskbar moves the window into its group when it does.
  **Still missing for full parity:** Xwayland **keyboard grabs** (in no wlroots
  release), **XDND** drag-and-drop (wlroots 0.21), and focus-stealing prevention
  (wlroots gives no timestamp); see
  [`21-r5-protocol-parity-plan.md`](21-r5-protocol-parity-plan.md).
- **Native-app protocol matrix (G7).** **Mostly done**: the compositor advertises
  (and wlroots implements) `xdg-activation`, `xdg-foreign` (v1 + v2), primary
  selection, `wlr`+`ext` data-control, `keyboard-shortcuts-inhibit`,
  `relative-pointer`, `presentation-time`, `fractional-scale` + `wp-viewporter`,
  `content-type`, `tearing-control`, `cursor-shape`, `xdg-wm-dialog`,
  single-pixel-buffer and alpha-modifier, alongside the workspace / foreign-
  toplevel / capture globals. Verified by `scripts/run-protocol-matrix-test.sh`.
  `pointer-constraints` (mouse lock/confine for games) is served too: the
  compositor activates the constraint of the surface under the cursor and enforces
  it in the cursor-motion path (locked to the client's hint, or confined to its
  region), the only part wlroots leaves to the compositor.
  `pointer-gestures` and **touch** input (routed through the cursor with the drag
  icon following a touch point) are served, along with **switch** devices
  (lid/tablet-mode), **pen/tablet** (`zwp-tablet-v2`) and **tablet-pad**
  (`zwp-tablet-pad-v2`). The headless harness injects touch and pen through an
  SDE test-only protocol (`serenity-virtual-input`);
  `scripts/run-virtual-touch-test.sh` covers touch routing and the touch-drag
  path.
  **Explicit drag-and-drop between windows is now
  served** (compositor-routed data-device DnD with a drag icon), and the port
  bridges LibGUI's `DragOperation` onto it both ways, so SerenityOS applications
  offer and accept drag-and-drop interoperably with standard Wayland clients.
  **Still absent:** `xdg-toplevel-drag` (not present in the pinned wlroots); and
  accessibility (AT-SPI). **Text input / IME is now served**: `text-input-v3` +
  `input-method-v2` (see [`22-input-method.md`](22-input-method.md),
  `scripts/run-ime-test.sh`).
- **`xdg-desktop-portal` (G7).** Phase 1 landed: **`xdg-desktop-portal-serenity`**
  (see [`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md)),
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
  ([`17-input-capture.md`](17-input-capture.md)), and the **USB** portal's backend
  is a real broker (policy + user prompt + remembered grants,
  `tools/portal/usb.cpp`, `scripts/run-usb-portal-test.sh`).
- **Text input / IME (G5): done (R5); accessibility (G1) partly.** `text-input-v3`
  / `input-method-v2` are served (see [`22-input-method.md`](22-input-method.md)),
  so an input method can compose CJK text into a focused field. **Sticky keys and
  high contrast are now implemented** (`[Accessibility] StickyKeys` / `HighContrast`,
  `scripts/run-sticky-keys-test.sh` and `scripts/run-high-contrast-test.sh`); AT-SPI
  and the magnifier remain, planned in [`29-accessibility.md`](29-accessibility.md).

## R6: Hardening & packaging (in progress)

Compositor-facing detail lives in [`08-compositor-gaps.md`](08-compositor-gaps.md) §5–7.

- **Multi-output / hotplug**: the compositor handles several outputs
  (`server_new_output`, `wlr_output_layout`), applies a saved per-output layout
  (mode/scale/position from `screenlayout.ini`), and implements
  `zwlr_output_management_v1` (apply and test), persisting what a client changes.
  **Losing an output no longer strands windows**: a window whose output is
  disabled or unplugged is moved onto a surviving output, keeping its offset from
  the old one, and the primary output is reassigned. Before this, those windows
  sat outside every output (invisible, unreachable without editing the layout) and
  every later placement used a destroyed output. Covered by
  `scripts/run-output-hotplug-test.sh`, which drives the disable through
  `zwlr_output_management_v1` like Display Settings or `wlr-randr` would; a real
  unplug arrives through the same helper. **HiDPI (integer and fractional output
  scaling) is done**; see "Recently resolved" below.
  **Still basic:** a re-enabled output does not become primary again (Serenity's
  `MainScreen` index would say otherwise), a maximised window keeps its old size
  when it moves across outputs of different sizes, and per-output mode/scale has
  no *UI*: DisplaySettings' Monitor tab is still inert
  (`libgui-wayland` patch 0020).
- **Multi-GPU / render-device selection; color management; direct scanout.**
- **Renderer selection & fallback (N2/N5): done.** `[Output] Renderer`
  (`auto`/`gles2`/`vulkan`/`pixman`, with the `SDE_RENDERER` override) selects the
  wlroots renderer, and a failed GPU path falls back to the software (pixman)
  renderer instead of ending the session; a full software mode is
  `SDE_RENDERER=pixman` + `LIBGL_GPU_DRIVER=softgpu`. Covered by
  `scripts/run-headless-test.sh`.
- **Performance budget & latency instrumentation (N3): done.** The compositor
  meters its frame time and the input → present interval (always on; reported via
  `SDE_PERF_STATS`/`SDE_PERF_LOG`), the budget is documented in
  [`25-performance.md`](25-performance.md), and `scripts/run-perf-test.sh`
  enforces it headless with the **software (pixman)** renderer. The budget is one
  refresh interval of the output being rendered on, so it is expressed in Hz: the
  test holds the compositor to a **240 Hz** interval (4.167 ms) at p99, runs a
  240 Hz output (restarting it with `SDE_OUTPUT_REFRESH=240`) and asserts the
  **cadence** as well as the work, checks input → present p95 inside 20 ms, and
  requires **no frames while idle** (rendering on damage only). One ~5 ms frame in
  a 244-frame drag on the software-GL path is the remaining item before an
  unconditional 240 Hz claim there; a per-hardware/refresh CI matrix and
  input-to-photon on a real DRM output remain future work. **Memory** is metered
  the same way (RSS, high-water, per-second growth) and guarded by the test's
  memory phase; `auto` no longer initialises software GL when there is no DRM
  device, which saves ~84 MB (~70 %) idle on a headless session. The meter also
  reports **per-process PSS**, the session's "who is using memory" report: the
  Taskbar, the Desktop and the applets at ~2.2 MB each and the services it spawns
  (the AudioServer at ~0.3 MB), enforced by `scripts/run-session-memory-test.sh`.
  The Audio applet's old ~9 MB came from two things, both fixed: it linked
  liblagom-audio (→ libpulse + libsndfile) for a backend it never used, so SDE now
  builds a protocol-only `SDEAudioClient` for it; and, the larger half, the shim
  substituted the *screen size* for its unsized window, so it allocated three
  1280×720 buffers before being reconfigured to 16×16; the placeholder now
  depends on the window type. The SDE's AudioServer is PipeWire-only too (it linked
  liblagom-audio for three header-only helpers). Session PSS: 53.9 → 46.6 MB (see
  §4 of the doc). The applets themselves have nothing left: all seven are uniform
  at 2.1–2.4 MB, and §4 now splits the whole session by what the pages are: the
  remaining levers are the pixel surfaces (32 %), the framework's per-client
  private cost, and the Taskbar's 1.8 MB heap. That Taskbar heap turned out to be
  a startup *design* cost, not a leak (§4: ~0.6 MB of it is the system menu at
  8.7 kB per installed app, linear); investigating it did surface a real bug: a
  pinned quick-launch entry whose `.af` is missing crashed the whole shell, fixed
  by `patches/0009-taskbar-quicklaunch-missing-app.patch` and covered by
  `scripts/run-taskbar-quicklaunch-test.sh`.
- **Crash recovery / watchdog / safe mode (N4): done.** The R4.2 supervisor
  restarts a *crashed* compositor; R6 adds a **watchdog** for a *wedged* one (the
  compositor writes a liveness heartbeat, `SDE_HEARTBEAT_FILE`; the supervisor
  restarts it when the heartbeat goes stale) and **safe mode** (after the restart
  budget, drop to a minimal shell instead of ending the session). User-facing
  session policy is configurable in `$XDG_CONFIG_HOME/sde/session.conf`
  (environment overrides). Covered by `run-session-test.sh` phases (H)–(M) and
  the heartbeat assertion in `run-headless-test.sh`. **Full session restore**
  (relaunching the user's windows after a compositor restart) is done; see the
  R4.2 note above.
- **CI (N6): green.** `scripts/run-all-tests.sh` runs the 88 `run-*-test.sh`
  tests by tier (every test is headless by design), `.github/workflows/ci.yml`
  gates a push on the fast tier and runs the rest nightly, and
  `ci/Containerfile` bakes the dependencies, wlroots and the ~1.2 GB Lagom
  framework build into an image so a job only builds the SDE. Both repositories
  are public, so standard GitHub-hosted runners are free and unlimited (never use
  a *larger* runner; those are always billed). Each test gets its own
  `SDE_SESSION_DIR` (which fixed `run-setting-test.sh`'s cross-session failure)
  and `SDE_XWAYLAND=0`; the five X11 tests re-exec in a private network namespace,
  which is what makes them safe, and runnable, next to a live session. That work
  also found and fixed a real compositor crash (a dead Xwayland taking the session
  down, §R5 above). See [`26-ci.md`](26-ci.md).
  **Landing since:** the image builds green, and the fast tier runs against it rather
  than failing before it starts. Getting there meant fixing four classes of
  CI-only failure, all the same shape: a fresh configure or a clean environment
  differing from a development machine: a font root and a headless backend the
  session had been supplying, build output that was being discarded (so failures
  printed a line and a status and nothing else), and three patch-set gaps
  (`26-ci.md` §6.6–6.7). The applications now build in the smoke job, which is what
  lets a third of the tier run at all.

  **Green since 2026-10-16:** the fast tier on every push (**82 passed, 0 failed,
  4 skipped, 0 timed out**) and the full suite nightly/on dispatch (**97 passed, 0
  failed, 4 skipped, 0 timed out**), with the informational budgets job passing. The
  four skips are environment-gated rather than gaps: no camera,
  `perf_event_open` blocked in the container, and the two screencast tests that
  need a real Wayland/DRM host (so `run-profiler-test.sh`'s SKIP is exercised for
  exactly the reason it was written). The X11 `WM_CLASS` grouping test passes.

  The largest single cause of the red runs was not a test at all: the image was
  **silently missing framework patches** `0017`, `0042` and `0045`, because the
  patch loop counted a patch it could neither apply nor reverse-apply as "assumed
  applied". A developer tree therefore had `/res/...` directory relocation and CI
  did not, which is why four unrelated-looking tests (settings, devtools, games,
  apps-smoke) failed with logs that said nothing. Both patch loops now **fail** on
  an unverifiable patch instead of assuming. Remaining known flakiness, both
  recorded in [`26-ci.md`](26-ci.md): `run-input-test.sh` occasionally misses the
  Alt release after Alt+Tab, and `libwm-setting-test` has an intermittent segfault.
- **Terminal keys vs the host's conventions.** Plain backspace emitted 0x08 where a
  host line discipline expects 0x7f (`erase = ^?`), so it did nothing in any
  raw-mode program (`yay`, `htop`, `vim`); Ctrl+letter is now converted to the
  control character in the LibWM shim, where Serenity's WindowServer used to do it.
  See [`27-terminal-keys.md`](27-terminal-keys.md): both were measured in a
  headless harness afterwards (`scripts/run-terminal-keys-test.sh`), and the Ctrl-C
  symptom turned out to be a *signal-disposition* bug rather than an encoding one.
- **Packaging** of `WindowServer` and the session; **sanitizers/fuzzing**;
  **compositor design doc** and observability.

## R1: Compositor bring-up (done)

- **Real-hardware leg: exercised.** The DRM/KMS + libinput path
  (`wlr_backend_autocreate` + `scripts/start-sde-session.sh`, with `libseat`
  granting DRM master and handling VT switching) runs on a real TTY, alongside the
  headless (`scripts/run-headless-test.sh`) and nested
  (`scripts/start-sde-nested.sh`) paths. The live-TTY ScreenCast legs were verified
  on this path ([`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md) §6).

## Recently resolved (kept so we don't re-chase them)

- **R3 finished: launch/stack animations, the stack-switch overlay, and the
  icon-reload hotkey.** `serenity_toplevel.set_launch_origin` carries the rect the
  launcher handed the app (SerenityOS's `__libgui_launch_origin_rect`, read by
  `GUI::Window` and now forwarded by the LibWM shim), and the compositor grows the
  window out of it (`start_launch_animation`); switching stacks slides the
  outgoing/incoming stacks (`Compositor`'s transition) and briefly shows the
  workspace grid with the current stack highlighted (the
  `WindowStackSwitchOverlay`); and `Ctrl+Super+Shift+I` reloads the
  output-scale-dependent chrome/icon bitmaps
  (`WindowManager::reload_icon_bitmaps_after_scale_change`). All are gated on
  `[Effects] AnimateWindows` (except the overlay, which SerenityOS always shows),
  and minimise/restore now share one outline-animation helper. Covered by
  `scripts/run-launch-animation-test.sh`, `run-stack-overlay-test.sh`,
  `run-stack-transition-test.sh` and `run-icon-reload-test.sh`.

- **Full session restore (R4.2): the user's applications are relaunched.** The
  compositor already served `xdg-session-management-v1` and persisted a
  session-managed toplevel's geometry, but nothing brought the applications
  themselves back after a compositor restart. `SessionManager::update_running_apps`
  now keeps `$SDE_STATE_DIR/session-apps` (one app id per line: the apps with a
  mapped toplevel, rewritten on map/unmap/destroy), and the session supervisor
  relaunches them once the socket is up (`run_restore`, `SDE_SESSION_RESTORE`),
  skipping apps the session's own startup command launches and ones that are no
  longer installed. Covered by `run-session-management-test.sh` (the app is
  recorded while its window is up) and `run-session-test.sh` phase (M).

- **A client's "no wallpaper" broke its own connection (R2/R3).** When the Desktop
  had no wallpaper it still called `serenity_window_manager.set_wallpaper` with
  `fd = -1`; libwayland cannot dup a -1 and aborts the *whole* request, so the
  client's connection was left broken and the desktop layer surface never mapped
  on a fresh state directory (the shell came up with no desktop). The
  `WaylandClient` shim now hands over a valid placeholder fd for the zero-size "no
  wallpaper" case, and the compositor closes the fd it no longer used.
  `scripts/run-shell-start-test.sh` covers it.

- **Fragile windows and the Super+H cursor highlight (R3).** SerenityOS closes
  its "fragile" windows (popups, menus, tooltips) as soon as focus moves; SDE's
  client popups are client menus (`serenity-menu` layer surfaces), so the
  compositor asks their owner to dismiss them (`dismiss_fragile_windows()`) on a
  window activation, while the window switcher is open, and on a workspace
  switch, the same three call sites as SerenityOS's
  `request_close_fragile_windows`. `Super+H` now toggles the cursor highlight
  (R4.7), like SerenityOS's `WindowManager`: it flips the configured state and
  falls back to SerenityOS's defaults (radius 25, red at alpha 110) when Mouse
  Settings have not configured one, and is not taken as a Super tap. Covered by
  `scripts/run-fragile-window-test.sh` and
  `scripts/run-cursor-highlight-test.sh`.

- **Exactly one window is active at a time (R3/R5).** The compositor deactivated
  the previous window only when the seat still reported a focused surface. A seat
  with no keyboard (headless, or a session's first windows) reports none, so
  every window kept its active frame (and raised Taskbar button) as it mapped even
  though it was never focused; the desktop scene showed several windows with
  active titlebars and no window marked active in the Taskbar. Focus now scans the
  window list (`deactivate_other_windows` in `WindowServer/src/Focus.cpp`) and
  reports the change to the WindowManagerServer, so exactly one window is active
  and the Taskbar's active/sunken button states follow. Covered by
  `scripts/run-active-window-test.sh`.

- **SerenityOS window management: drag, unmaximize-on-move, Alt+Shift, animations
  (R3).** `Super`+left-drag moves a window and `Super`+right-drag resizes it from
  anywhere (the 3×3 hot area under the cursor picks the edges); dragging a
  maximized window restores its size first, keeping the cursor's horizontal
  fraction; `Alt+Shift` cycles the configured keyboard layouts; and minimize/
  restore animate the SerenityOS outline between the window and its Taskbar
  button (the Taskbar's `set_window_taskbar_rect` is now consumed), gated on
  `[Effects] AnimateWindows`. Covered by `scripts/run-window-animation-test.sh`;
  the launch and window-stack-transition animations remain (see the WM gaps
  above).

- **`on_demand` layer surfaces now take keyboard focus (R5).** Layer surfaces
  with `zwlr_layer_surface_v1` keyboard interactivity `on_demand` (client popups
  such as the Wi-Fi network list and its passphrase field, or a ComboBox dropdown)
  were never given keyboard focus by the compositor, so their text fields could
  not be typed into (keys went to whatever toplevel had focus). The compositor now
  focuses such a surface when it maps (and on commit), and restores focus to the
  topmost toplevel when it unmaps; `focus_toplevel` clears the layer focus. Panels,
  the desktop and applets use `NONE` and are unaffected. See
  `WindowServer/src/LayerShell.cpp` (`focus_layer_surface`).

- **Wi-Fi applet (G5/W6).** `WifiApplet` is a LibGUI frontend to **NetworkManager**
  over the system D-Bus (libdbus + `Core::Notifier`), modeled on KDE Plasma's
  Networks applet: the Breeze signal-arc tray icon, a tooltip, and a popup with a
  Wi-Fi switch, the network list, connect/disconnect, an inline passphrase prompt
  and a `SecretAgent`. It sits in the applet area to the left of the battery
  applet (the compositor appends `WiFi` then `Battery` to `[Applet] Order`). See
  [`19-wifi-applet.md`](19-wifi-applet.md); covered by
  `scripts/run-wifi-applet-test.sh` (a private bus + a mock NetworkManager).

- **Every Settings dialog launches and its settings apply (R4).** The Settings
  grid lists 13 `Category=Settings` apps. A launch + backend sweep
  (`scripts/run-settings-test.sh`) found three that could not open at all and two
  that opened with empty lists, all host-port gaps in `libgui-wayland` patches
  `0044`–`0047`:

  | Dialog | Problem | Fix |
  | --- | --- | --- |
  | Certificate Settings | binary absent from the Lagom app list, so the launcher could not exec it | build `CertificateSettings` (`0044`) |
  | Network Settings | aborts reading SerenityOS's `/sys/kernel/net/adapters` | enumerate host interfaces with `getifaddrs()` (`0047`) |
  | Browser Settings | aborts on a fresh profile: the content-filter / autoplay list file does not exist yet | treat a missing list as empty (`0046`) |
  | Maps Settings | provider list empty: `/usr/share/Maps/TileProviders.json` was not relocated under `Base` | relocate `/etc` + `/usr/share` when the file exists beneath `Base` (`0045`) |
  | Games Settings | card-back list empty and watch failed: `FileSystemModel`/`FileWatcher` bypass `Core::System` path relocation | relocate in both (`0045`) |

  Phase A of the test launches every dialog (13 settings apps + the Settings
  launcher + Theme Editor) and asserts each maps a window with no fault; Phase B
  exercises the backends the dialogs drive (`run-input-settings-test.sh` for
  mouse + keyboard, themes, fonts, output/monitor, workspaces, effects,
  config/background, and the Settings portal). Clock, Calendar, Mail, Terminal,
  Users, Display and Debug Settings launched cleanly already.

- **Shell startup race (SDDM login: applets but no Taskbar/desktop).** The
  compositor spawned its `-s` startup command *before* it started listening on the
  WindowManagerServer socket. The Taskbar, WorkspacePicker and Keymap connect to
  `/tmp/portal/wm` at startup, so on a real login one of them could lose the race
  and abort with `VERIFICATION FAILED` in
  `ConnectionToWindowManagerServer::the()`, leaving a session with only the
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
  (host M8 resolved; verified: a Serenity client presents `@2x` on a 2x/1.5x
  output). A saved size with no matching mode requests a **custom mode**, so
  `scripts/run-desktop-scene.sh --headless --scale 2` captures a 2560x1440 HiDPI
  scene. `DisplaySettings`' Monitor tab is **live**: Resolution, Position (X/Y) and
  the **Display scale** control (1x/1.25x/1.5x/2x) apply on **Apply**: the shim
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
  heartbeat (`SDE_HEARTBEAT_FILE`, a `Core::Timer` touching the file each
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
  (SDE patch 0004-profiler-libserenitysystem); covered by `scripts/run-profiler-test.sh`. Timestamps must be
  **milliseconds** (`Profile::length_in_ms()` is a raw difference used as a
  bucket count); nanoseconds ballooned RSS to several GiB.

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
- **ComboBox dropdown**: regression test added (`scripts/run-combo-test.sh`, `libwm-combo-test`): opening a ComboBox popup (the Display Settings wallpaper "Mode" box) must produce a real popup, not a collapsed sliver.
- **Desktop ▸ "Display Settings" now opens DisplaySettings.** `Desktop::Launcher::open("/bin/DisplaySettings")` (the desktop right-click entry) resolved the URL by file extension, landing on the wildcard handler (`/bin/TextEditor`), and `stat`ed the host path `/bin/DisplaySettings`, which does not exist, so nothing opened. The port's `LaunchServerConnection::open_url` now runs an **executable regular file** directly (mirroring the upstream `LaunchServer`), mapping Serenity's `/bin/X` to the session app dir like the rest of the port. The desktop entry now launches the same DisplaySettings dialog the Settings app spawns. Verified by `scripts/run-launch-exec-test.sh`.
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
  (`serenity_window_manager.dismiss_menus`), instead of appearing stuck open,
  verified by `scripts/run-menu-dismiss-test.sh`.
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
- **`ImageDecoder` portal**: served in-process by libgui-wayland (its in-process
  fallback) and, when SDE runs, by the `ImageDecoder/` session service. This fixes
  the `FileSystemModel`/wallpaper thumbnails (`connect: No such file or
  directory`) and lets **ImageViewer** decode images. The in-process host decodes
  **inline on its own thread**: `ImageDecoder`'s upstream job runs on
  `Threading::BackgroundAction`, the single process-global thread that
  `FileSystemModel`'s thumbnail renderer already occupies, so sharing it deadlocked
  thumbnails; a separate process does not. See `06-service-layering.md` §4.
- **Out-of-process web engine**: `WebContent` + `RequestServer` are built by
  `libgui-wayland` and spawned on demand by `LibWM` (a socketpair handed over via
  `SOCKET_TAKEOVER`); `SQLServer` is hosted with a listening socket handed over
  (see `07-systemserver-roles.md` §2.3). **Help** renders the SerenityOS man pages
  (relocated from `Base/usr/share/man`); **Browser** loads pages, including
  `https://`; HTTPS verifies against the host trust store (LibTLS patch `0024`).
  The session seeds the skeleton user config (`scripts/seed-serenity-home.sh`) that
  `Browser` reads at startup. Known limit: LibTLS cannot parse ECDSA roots
  (`id-ecPublicKey`), so it skips them; RSA roots load and verify fine.
- Documented: bitmap-font zoom limitation (`02-compositor-decision.md` §11).
