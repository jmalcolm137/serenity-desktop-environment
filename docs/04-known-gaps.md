# Serenity Desktop Environment — Known Gaps & Remaining Work

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md)

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

## R3 — Serenity look & behaviours (chrome done, rest pending)

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

- **Session lifecycle (G6 / W5).** No greet/login, session start, autostart,
  save/restore, lock, logout, reboot, or shut-down; no SystemServer-style
  supervision.
- **Settings & appearance (G5 / W6).** No display (mode/scale/rotation), input,
  appearance, cursor or wallpaper settings backends. `SDE_WALLPAPER` /
  `SDE_WALLPAPER_MODE` environment variables are a deliberate stand-in (see
  `02-compositor-decision.md` §11). Fonts are fixed to the bitmap Katica family,
  so size-based zoom is a no-op.
- **Application enablement (W7).** `SystemMonitor` (needs `/proc`), `MouseSettings`,
  `Debugger`, `CrashReporter` feasibility not yet addressed. `Terminal` runs.
- **Web engine / `WebContent` service (W7, host).** `Help` and `Browser` are
  WebView apps; they abort because `OutOfProcessWebView` cannot create its
  `WebContentClient` — the out-of-process **`WebContent`** service (LibWeb host +
  IPC) is not built or wired. Running it (and serving it in-process alongside the
  other portals) is a milestone of its own. **Help** in the Taskbar's system menu
  depends on this.

## R5 — Hardening & packaging (not started)

- **XWayland (G7)** — not implemented; native Wayland apps work via `xdg-shell`.
- **HiDPI (integer/fractional) and multi-output** — not done (LibWindowServer M8
  soft-fractional-scaling issue still open).
- **Packaging** of `WindowServer` and the session; **headless CI matrix**.

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
- Port (host): empty-size window `VERIFY` (Desktop); first layer configure not
  delivered, so the Desktop widget tree never laid out; `Process::spawn`
  working-directory `TODO()` crash; LaunchServer stub returned invalid
  responses (broke the file context menu); `FileIconProvider` couldn't find its
  patterns (`/etc` absent from the sparse checkout); Serenity `/bin/X` helper
  paths not resolved to host binaries (launching failed with ENOENT); Launch
  service implemented (handler resolution + spawn).
- Documented: bitmap-font zoom limitation (`02-compositor-decision.md` §11).
