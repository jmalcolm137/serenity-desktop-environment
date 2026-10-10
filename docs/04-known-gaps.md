# Serenity Desktop Environment — Known Gaps & Remaining Work

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`05-r3-plan.md`](05-r3-plan.md) · [`06-compositor-gaps.md`](06-compositor-gaps.md)

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

## R3 — Serenity look & behaviours (chrome done, rest pending)

- **Menus opened from layer surfaces don't grab input (G3 / W2).** The Taskbar's
  (and Desktop's) menus are overlay layer surfaces; unlike the `xdg_popup` menus
  opened from ordinary windows (which grab), they do not take an input grab, so a
  click on *another* surface does not dismiss them — the menu appears "stuck
  open". It closes only on selecting an item, Escape, or a click on the owner's
  own surface. Needs a compositor-side grab (or an outside-press dismissal) for
  client menus.
- **Window switcher with live thumbnails** — not implemented.
- **Snapping** — not implemented.
- **Workspaces** — not implemented; workspace IDs are no-ops.
- **Notifications** — not implemented (NotificationServer is a stub).
- **Tooltips** — not implemented. `GUI::Application::show_tooltip()` is a no-op in
  the port, so the Taskbar quick-launch icons, window-title buttons, clock,
  Show-Desktop button and applets show no hover text (all of which SerenityOS
  provides). Needs a tooltip window near the widget/cursor.
- **Applet area / system tray parity** — applets run as layer surfaces, but
  applet-area position/semantics are no-ops.
- **Font parity** — fixed to the bitmap Katica family; size-based zoom is a no-op.
  Theme switching (Taskbar ▸ Themes) works: the list is read under the resource
  root, the theme is loaded/recoloured in-process, and a shared broker file
  (`$XDG_RUNTIME_DIR/sde-theme`) is watched by every client and the compositor so
  a switch restyles all running apps and the window chrome. As in SerenityOS
  (`keep_desktop_background=false` clears `Background/Color`), a theme switch
  changes the desktop **colour** but does not replace a wallpaper **image** — a
  set wallpaper stays and covers the colour. This is intentional parity, not a
  bug.

## R4 — Session & settings (not started)

Compositor-facing session/input gaps (logind contract, idle/inhibit/lock/DPMS,
keymap/IME, input-device config) are catalogued in
[`06-compositor-gaps.md`](06-compositor-gaps.md) §1 and §4.

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
- **Application enablement (W7).** `SystemMonitor` (needs `/proc`), `MouseSettings`,
  `Debugger`, `CrashReporter` feasibility not yet addressed. `Terminal` runs.
- **Web engine / `WebContent` service (W7, host).** `Help` and `Browser` are
  WebView apps; they abort because `OutOfProcessWebView` cannot create its
  `WebContentClient` — the out-of-process **`WebContent`** service (LibWeb host +
  IPC) is not built or wired. Running it (and serving it in-process alongside the
  other portals) is a milestone of its own. **Help** in the Taskbar's system menu
  depends on this.

## R5 — Compatibility & ecosystem (not started)

Compositor-facing detail lives in [`06-compositor-gaps.md`](06-compositor-gaps.md) §3–4.

- **XWayland (G7)** — not implemented; native Wayland apps work via `xdg-shell`.
- **Native-app protocol matrix (G7).** `xdg-activation`, `xdg-foreign`,
  `xdg-toplevel-drag`, `data-control`/primary selection, drag-and-drop,
  `keyboard-shortcuts-inhibit`, `relative-pointer`/`pointer-constraints`/
  `pointer-gestures`, `presentation-time`, `fractional-scale`/`wp-viewporter`,
  `content-type`, `tearing-control` are all absent.
- **Portals (G7).** No screenshot/screencast/settings/global-shortcuts/
  remote-desktop/input-capture portal strategy.
- **Text input / IME (G5) and accessibility (G1).** `text-input-v3` /
  `input-method-v2`, AT-SPI, sticky keys, magnifier and high contrast are absent.

## R6 — Hardening & packaging (not started)

Compositor-facing detail lives in [`06-compositor-gaps.md`](06-compositor-gaps.md) §5–7.

- **HiDPI (integer/fractional) and multi-output** — not done (LibWindowServer M8
  soft-fractional-scaling issue still open — a host dependency, not compositor
  code).
- **Multi-GPU / render-device selection; color management; direct scanout.**
- **Performance budget & latency instrumentation (N3).** No budget, frame-pacing
  plan or perf-regression test, despite N3 targeting "low predictable input
  latency, 60 Hz".
- **Crash recovery / watchdog / safe mode (N4)** — no supervisor, restart or
  session restore; a compositor crash loses the session.
- **Packaging** of `WindowServer` and the session; **headless CI matrix**;
  **sanitizers/fuzzing**; **compositor design doc** and observability.

## R1 — Compositor bring-up (done, one leg unexercised)

- **Real-hardware leg.** The DRM/KMS + libinput path is implemented
  (`wlr_backend_autocreate` + `scripts/start-sde-session.sh`) but has only been
  exercised **nested** on a host Wayland session; a real TTY/DRM run has not been
  performed. Headless is covered by `scripts/run-headless-test.sh`.

## Recently resolved (kept so we don't re-chase them)

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
- **`ImageDecoder` portal** — served in-process by libgui-wayland
  (`LibWM` hosts SerenityOS's `ImageDecoder::ConnectionFromClient` on the `image`
  socketpair). This fixes the `FileSystemModel`/wallpaper thumbnails
  (`connect: No such file or directory`) and lets **ImageViewer** decode images.
  Note the in-process host decodes **inline on its own thread**: `ImageDecoder`'s
  upstream job runs on `Threading::BackgroundAction`, the single process-global
  thread that `FileSystemModel`'s thumbnail renderer already occupies, so sharing
  it deadlocked thumbnails. No SDE session service: see
  `06-service-layering.md` §4 for why (the upstream service is a single-client
  system service; a session instance would be a sharing optimisation only).
- Documented: bitmap-font zoom limitation (`02-compositor-decision.md` §11).
