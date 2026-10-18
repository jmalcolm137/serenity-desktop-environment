# Serenity Desktop Environment on Linux: Objectives

**Status:** Draft v0.1 · **Date:** 2026-10-09
**Related:** [`02-compositor-decision.md`](02-compositor-decision.md) · [`libgui-wayland` DESIGN.md](https://github.com/jmalcolm137/libgui-wayland/blob/main/DESIGN.md)

---

## 1. Vision

Bring the **SerenityOS desktop experience to Linux** as a first-class Wayland
session: a faithful, coherent, late-1990s-workstation feel: one consistent
theme, one font family, a taskbar and applets, classic server-drawn window
frames, an alt-tab switcher, and a suite of applications that all share a
single design language.

The environment must run **unmodified SerenityOS `LibGUI`/`LibGfx`
applications** (already possible today via `libgui-wayland`/LibWindowServer) while
remaining a normal Linux desktop: it boots from a login manager, drives DRM/KMS
and libinput, and can host ordinary Wayland applications alongside SerenityOS
ones.

> Think *"the Serenity desktop, but the kernel and display stack are Linux."*
> Not a port of SerenityOS, and not an emulator: a real desktop environment that
> reuses SerenityOS's userland code where the boundary is a protocol, and
> supplies the system side that SerenityOS used to provide by itself.

## 2. Background

SerenityOS is deliberately monolithic. There is no X11 and no Wayland: a widget
toolkit (`LibGUI`), a graphics library (`LibGfx`), the applications, and the
compositor all live in one system, and the boundary between an application and
the system is a **custom IPC protocol over a Unix socket to the `WindowServer`
service**.

`libgui-wayland` has already proven the central idea: because `LibGUI`'s entire
platform surface is that protocol, you can keep `LibGUI`, `LibGfx`, and the
applications **byte-for-byte unmodified** and replace the *far side* of the
protocol (the WindowServer) with a library (`LibWindowServer`) that speaks the
WindowServer protocol on one side and Wayland on the other. That project runs
44 of the 49 applications in `Userland/Applications`; the SDE adds the
out-of-process services they need (the `AudioServer`, docs/06).

What LibWindowServer explicitly does **not** do (because on Wayland it is the
compositor's job) is everything *global*: window management, the shell, the
taskbar, applets, alt-tab, workspaces, wallpaper, global shortcuts, screen
capture, display/input configuration, the session/login lifecycle, and the
system services that a handful of applications still need. **That missing
system side is this project.**

## 3. What we are building (definition)

A **session** composed of:

1. **A Wayland compositor**: `WindowServer` (the subject of the
   companion decision document).
2. **A desktop shell**: the *actual* SerenityOS shell applications and widgets:
   the `Userland/Services/Taskbar` panel (system-menu/start button, quick-launch,
   window list, applet area, clock, show-desktop), its applets, the Desktop, and
   the settings apps. Same widgets, same behaviour; not a redesign.
3. **System services**: the Serenity services that applications expect, either
   as in-process shims (following the LibWindowServer pattern) or as session daemons:
   WindowManagerServer, Clipboard, Config, Launch, Notification, FileSystemAccess,
   ImageDecoder, AudioManager, and a Serenity-compatible pseudo-terminal for
   `Terminal`.
4. **A session manager**: login/autostart, session save/restore, lock, logout,
   reboot/shutdown.
5. **Settings and per-compositor backends**: display, input/pointer, keyboard,
   appearance, cursor, wallpaper.

Together these make a Serenity app (`Calculator`, `TextEditor`, `PDFViewer`,
`FileManager`, `Piano`, …) feel at home while Linux apps are still usable.

## 4. Goals

### 4.1 Functional goals

| # | Goal | Notes |
|---|---|---|
| **G1** | **Faithful shell chrome: the same widgets and behaviour as SerenityOS.** Reuse the real shell applications: `Userland/Services/Taskbar` (system-menu/start button, quick-launch, window list, applet area, clock, show-desktop), its applets, the Desktop and Settings. | Structural and behavioural parity from the actual SerenityOS code, not a bespoke panel. Widget painting comes from the `LibGfx` `WindowTheme`; window frames are compositor-drawn. |
| **G2** | **Run unmodified Serenity applications, including the shell apps.** Reuse `libgui-wayland` (LibWindowServer) and run the real `Taskbar`, applets and settings; add the remaining service shims the deferred apps need. | Preserve the "keep Serenity code unmodified" property end-to-end, shell included. |
| **G3** | **Serenity window-management semantics.** Stacking, focus, minimize/maximize/zoom, close, fullscreen, window snapping, workspaces, keyboard and mouse chording. | Server-side frames by default, with per-client `xdg-decoration` negotiation. |
| **G4** | **System services.** Clipboard, Config, Launch, Notification, FileSystemAccess, ImageDecoder, Audio(Manager), pty/Terminal, and the pieces `SystemMonitor`/`MouseSettings`/`Debugger`/`CrashReporter` need where feasible. | Serve the Serenity protocol; implement against Linux/native APIs underneath. |
| **G5** | **Settings and appearance.** Serenity theme/color-scheme/fonts; MouseSettings; display configuration (mode/scale/rotation); input/pointer settings; cursor theme/size; wallpaper. | Write paths sit behind a per-compositor backend interface. |
| **G6** | **Session lifecycle.** Greet/log in, start session, autostart session apps, restore sessions, lock, log out, reboot, shut down. | Maps to Linux login/session/DRM conventions. |
| **G7** | **Coexist with Linux apps.** Run native Wayland apps; provide XWayland for X11 apps where the compositor supports it. | Our apps must not be the only thing that runs. |

### 4.2 Non-functional goals

| # | Goal | Notes |
|---|---|---|
| **N1** | **Fidelity to Serenity look & feel, and to Serenity code conventions.** | AK types, `ErrorOr`/`TRY`, `dbgln`, snake_case, west `const`, 4-space indent. Our code should read as if it belonged in `Userland/`. |
| **N2** | **Maximise reuse of unmodified Serenity source** at protocol boundaries. | Prefer a shim over a patch; any patch should be small, general, and upstreamable. |
| **N3** | **Performance.** Low, predictable input latency; smooth 60 Hz; a software compositor fallback. | Client rendering in LibGUI/LibGfx is CPU by construction; the compositor composites `wl_shm` with a GPU renderer and keeps a Pixman fallback. The budget, meters and the enforcing test are in [`25-performance.md`](25-performance.md). |
| **N4** | **Robustness.** A shell/WM fault should not take down running applications or lose the session where avoidable. | Fault isolation is a design input to the compositor decision. |
| **N5** | **Portability.** Multi-output, HiDPI (integer and fractional), common GPUs, headless for CI. | Follows the "compositor is the source of truth" principle. |
| **N6** | **Maintainability for a small team.** Lean on proven libraries for plumbing; own only what defines the experience. | A one- or two-person effort must be able to keep this alive. |
| **N7** | **Licensing.** Our code remains **BSD-2-Clause**; dependencies must be compatible, and GPL components may not be linked. | Matches SerenityOS and `libgui-wayland`. |
| **N8** | **Testability.** Headless, scripted, pixel-asserted tests where possible; CI matrix. | Continue the pattern established by `libgui-wayland`. |

## 5. Non-goals

- **Porting the SerenityOS kernel or syscall ABI.** Linux is the kernel.
- **GPU-accelerating the client rendering path.** `LibGUI` drawing is CPU by
  construction (`GUI::Painter` is a `Gfx::Painter`, and neither LibGUI nor
  LibGfx links `LibGPU`/`LibGL`). We do not add a GPU path to widgets or apps;
  GPU acceleration is confined to the compositor's composition/scanout.
  `LibSoftGPU`/`LibGPU` stay out of scope for the shell.
- **Bit-exact framebuffer parity.** Fonts are compared structurally, not
  glyph-for-glyph.
- **An X11 session.** X11 apps are supported only through XWayland, if at all.
- **Replacing the Linux kernel, DRM/KMS, libinput, or the login/Mesa stack.**
- **Blocking on upstream Serenity to add hooks.** We shim at protocol
  boundaries; we do not turn Serenity into a Linux app framework.
- **A single-compositor forever lock-in.** The compositor is chosen for the
  product, but shell clients stay protocol-portable and settings keep a backend
  seam.

## 6. Design principles

These extend and are consistent with LibWindowServer's existing principles.

- **P1: The compositor is the source of truth.** Never hardcode outputs, scale,
  seat, clipboard or window state; query the real compositor and share state
  back. Placeholder constants are scaffolding only.
- **P2: Shim at protocol boundaries; keep Serenity code unmodified.** The
  The WindowServer in-process shim and the out-of-process AudioServer service are
  the model. This includes the shell: the
  `Taskbar` and its applets are SerenityOS applications we run, not panel widgets
  we reimplement. Add a shim before a patch; keep any patch tiny and
  upstreamable.
- **P3: Use AK and SerenityOS conventions.** No `std::` where AK suffices; no
  third-party abstraction where AK provides the facility.
- **P4: Portable protocols first; per-compositor backends behind interfaces.**
  Read standard Wayland; write system settings through a thin backend that has
  one implementation per desktop/compositor.
- **P5: Own the experience, rent the plumbing.** Own the compositor policy and
  the shell (what makes it *Serenity*); lean on libraries (a compositor toolkit,
  input/output stacks, PipeWire, D-Bus) for the rest.
- **P6: Comprehensible and testable.** Follow Serenity's "understandable by one
  person" ethos; prefer small, scriptable, headless-testable pieces.
- **P7: BSD-2, upstream-friendly.** Every generally useful change is written to
  be proposed upstream.
- **P8: Wayland-native and interoperable.** Prefer the **standard** protocol:
  use Wayland's stable/staging `xdg_*`, `ext_*` and `wp_*` extensions wherever one
  exists, and fall back to `wlr-*` or an SDE-private protocol **only when there is
  none** (document why). Expose the standard globals so third-party clients work
  unmodified: panels (`waybar`), lockers (`swaylock`), idle daemons (`swayidle`),
  clipboard managers (`wl-clipboard`), portals, screenshot tools. And make
  our own components (lock client, idle daemon, settings backends) **portable
  clients** that run under other compositors without SDE-specific protocols. Keep
  the private surface (`serenity-window`) minimal and scoped to Serenity shell
  parity. See [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §2
  for the protocol posture.
- **P9: Serenity fidelity, through interoperable building blocks.** Where
  SerenityOS does something better than other desktops (a richer service, a
  distinct behaviour, a component others lack), we **reproduce Serenity's
  behaviour** rather than adopting another ecosystem's weaker equivalent. When
  that requires building an app or service ourselves, it must be a **standards-
  based Wayland/D-Bus citizen** (P8) that runs under other compositors too, and
  we use standard protocols, not another ecosystem's private ones (GNOME's/KDE's).
  Fidelity is the goal; interoperability is how we build it.

## 7. Architecture overview

```
 ┌───────────────────────────────────────────────────────────────────────────┐
 │ Applications                                                              │
 │   unmodified SerenityOS apps (Calculator, TextEditor, PDFViewer, …)        │
 │   native Wayland apps                          X11 apps (via XWayland)     │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ Userland compatibility layer                                              │
 │   LibGUI / LibGfx (unmodified)  ── LibWindowServer (WindowServer→Wayland)            │
 │   LibAudio (unmodified)         ── SDE AudioServer service (→PipeWire)    │
 │   service clients → in-process shims: Config, Launch, Clipboard, Notify,   │
 │   FileSystemAccess, ImageDecoder, …                                        │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ SerenityOS desktop shell apps (unmodified, Wayland clients)               │
 │   Taskbar (system menu · quick-launch · window list · clock) · applets     │
 │   Desktop · Settings · Notification banners · Wallpaper                    │
 │   ⇅ Serenity WindowServer + WindowManagerServer protocols ⇅               │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ WindowServer  (wlroots, C++)                                        │
 │   display server · window manager · SSD frames · switcher/overlays · proto │
 ├───────────────────────────────────────────────────────────────────────────┤
 │ Linux: DRM/KMS · libinput · PipeWire · D-Bus · logind · Mesa · XWayland    │
 └───────────────────────────────────────────────────────────────────────────┘
```

The **shell talks to the compositor through protocols** (layer-shell plus the
Serenity `WindowServer` and `WindowManagerServer` protocols, the latter served
by the WindowServer itself), not private calls, so the shell applications can be
reused unmodified and the compositor can evolve underneath them. The **app-like shell surfaces are clients**: Taskbar,
applets, Desktop, Settings, notifications. The compositor keeps the chrome that
SerenityOS implements *inside* `WindowServer` (server-side window frames, the
window switcher, and snap/resize overlays) because that is where the unmodified
SerenityOS code lives. See the companion document for protocol details.

## 8. Scope, constraints, assumptions

- **Target platform:** x86-64 Linux, DRM/KMS, libinput, Wayland 1.x.
- **Toolchain:** C++23 and SerenityOS **Lagom**, extended as in
  `libgui-wayland`; a pinned SerenityOS revision for reproducibility.
- **No root required** for development; the session ships under a user prefix,
  with an eventual installable package.
- **SerenityOS source is consumed at a pinned revision**; the boundary remains
  the WindowServer/AudioServer/etc. IPC protocols.
- **Software rendering is the baseline.** Accelerated rendering is an
  optimization, never a requirement.
- **The session must run headless** for CI, with a small test compositor.

## 9. Success criteria

The environment is considered successful when, on a stock Linux machine:

1. The user can start an **SDE session** (from a display/login manager or a
   nested session) and see a Serenity wallpaper, taskbar, and desktop.
2. **Unmodified Serenity applications** run (`Calculator`, `TextEditor`,
   `PDFViewer`, `FileManager`, `Piano`, `Browser`, …) with correct theming.
3. The **taskbar** lists open windows and can activate/minimize/close them; the
   **alt-tab switcher** shows live thumbnails; window frames are Serenity-style
   for both Serenity and native applications.
4. **Workspaces, snapping, and keyboard shortcuts** behave as in Serenity.
5. **`Terminal` and `SystemMonitor`** run, backed by Linux ptys and `/proc`.
6. **MouseSettings / DisplaySettings / AppearanceSettings** apply changes
   end-to-end through the compositor backend.
7. **Native Wayland apps** run beside Serenity apps.
8. **Headless CI** exercises the compositor, the shell and the app matrix, with
   scripted input and pixel assertions.

## 10. Workstreams

| W | Workstream | Owns |
|---|---|---|
| W1 | **Compositor** | display server, window management, protocols, window-type semantics, SSD frames, switcher/overlays, outputs, input. |
| W2 | **Shell** | run the unmodified SerenityOS Taskbar and applets; Desktop, Settings, wallpaper, notifications. |
| W3 | **LibWindowServer / WindowServer shim** | hardening, SSD/CSD policy, HiDPI, remaining portals; the app↔compositor boundary. |
| W4 | **System services** | WindowManagerServer, config, launch, clipboard, notify, filesystem, image, audio manager, pty. |
| W5 | **Session management** | login/autostart/save-restore/lock/logout/power, `SystemServer`-style supervision. |
| W6 | **Settings & backends** | per-compositor backend interface; display/input/appearance/cursor/wallpaper. |
| W7 | **Application enablement** | Terminal, SystemMonitor, MouseSettings, Debugger/CrashReporter feasibility. |
| W8 | **Build, packaging, CI** | Lagom extensions, packaging, headless matrix, docs. |

## 11. Milestones (high level)

| # | Milestone | Outcome |
|---|---|---|
| **R0** | **Design** | Objectives + compositor decision (these documents). |
| **R1** | **Compositor bring-up** | A minimal SDE compositor runs one LibWindowServer app, one layer-shell panel, one native app, on real hardware; frame-perfect and headless. |
| **R2** | **Shell v1** | Taskbar, clock, window list, system menu, wallpapers; WindowManagerServer. |
| **R3** | **Serenity look & behaviours** | SSD Serenity frames, switcher with thumbnails, snapping, workspaces, applets, notifications. |
| **R4** | **Session & settings** | Login/autostart/lock/power; display/input/appearance settings; Terminal + SystemMonitor, delivered as milestones **R4.1–R4.9** ([`11-r4-implementation-plan.md`](11-r4-implementation-plan.md)). |
| **R5** | **Compatibility & ecosystem** | XWayland; the native-app protocol matrix; portals; clipboard/DnD/data-control; text-input/IME; accessibility; global shortcuts; capture/recording. |
| **R6** | **Hardening & packaging** | HiDPI, multi-output, multi-GPU, performance/latency budgets, crash recovery/safe mode, packaging, CI matrix, docs. |

Each milestone is delivered with headless tests where the behaviour is
scriptable, and manual verification where it is not (menus, popups, IME).

R4's additional milestones:

| # | Milestone |
|---|---|
| **R4.1** | Session safety net: idle, inhibit, lock, DPMS (+ a lock client) |
| **R4.2** | Session lifecycle & supervisor: autostart, logout/reboot/shutdown, restart |
| **R4.3** | Portable session integration (logind/systemd optional) |
| **R4.4** | Compositor configuration & live reload |
| **R4.5** | Display / output settings (+ Monitor tab) |
| **R4.6** | Input settings & keymaps (+ MouseSettings) |
| **R4.7** | Appearance, effects & fonts (+ working zoom) |
| **R4.8** | Workspaces settings tab |
| **R4.9** | Applications enablement (Terminal, SystemMonitor, …) |

## 12. Key risks (summary)

Detailed analysis and mitigations live in the companion document; the headline
risks are:

- **Compositor effort and maturity**: writing/owning a compositor is the
  largest single risk. Mitigated by building on a proven toolkit and by a
  de-risking spike.
- **Fidelity vs. effort**: the gap between "looks Serenity" and "behaves
  Serenity" is mostly in window management and shell semantics, not rendering.
- **Deferred applications**: `Terminal`, `SystemMonitor`, `Debugger`,
  `CrashReporter`, `MouseSettings` need service shims or Linux equivalents whose
  feasibility varies. `Profiler` is **resolved**: a native `perf_event_open`
  sampler (with `PERF_SAMPLE_CALLCHAIN` user stacks) in `LibSerenitySystem`
  replaces SerenityOS's `profiling_enable` + `/proc/<pid>/perf_events`; see
  `04-known-gaps.md`.
- **Small-team sustainability**: mitigated by owning only the differentiated
  layers and renting the rest, and by keeping the shell protocol-portable.
- **Multi-output / hotplug**: coarse (HiDPI itself is done: the fractional
  output scale and the client backing store both landed). Per-output mode/scale
  and hotplug are basic. Direct scanout is automatic through the wlroots scene
  and is now instrumented; multi-GPU, colour/HDR and VRR validation remain
  hardware-gated (see `04-known-gaps.md`).

## 13. Glossary

- **LibWindowServer**: the in-process WindowServer-protocol shim from `libgui-wayland`.
- **AudioServer**: the SDE service serving the SerenityOS AudioServer/AudioManager
  protocols over the session portals, backed by PipeWire.
- **SSD / CSD**: server-side / client-side decorations.
- **WindowManagerServer**: the Serenity service the Taskbar and window
  switcher use to observe and control windows.
- **WindowManagerServer**: our implementation of that protocol over a Wayland
  window-management protocol.
- **Applet**: a small SerenityOS shell widget (workspace picker, CPU/memory/
  network graphs, network, clipboard history, audio, keymap) that runs as a
  client window and is laid out by the compositor's applet area.
- **Lagom**: SerenityOS's host-native build of its userland libraries.

## 14. References

- SerenityOS: <https://github.com/SerenityOS/serenity>; `Userland/Services/WindowServer/{WindowManager,WindowFrame,WindowSwitcher,AppletManager,Overlays,Compositor,ScreenLayout}.cpp`; `Userland/Services/WindowServer/{WindowServer,WindowManagerServer,WindowManagerClient}.ipc`.
- `libgui-wayland` / LibWindowServer: <https://github.com/jmalcolm137/libgui-wayland> (see `DESIGN.md`).
- Wayland protocols: <https://wayland.app/protocols/>; the canonical set is
  `wayland-protocols` (stable/staging), <https://gitlab.freedesktop.org/wayland/wayland-protocols>.
- `xdg-desktop-portal` (+ backends such as `xdg-desktop-portal-wlr`/`-gtk`):
  <https://flatpak.github.io/xdg-desktop-portal/>; the freedesktop D-Bus portals
  (FileChooser, Screenshot, ScreenCast, RemoteDesktop, InputCapture,
  GlobalShortcuts, **Settings**). Distinct from SerenityOS's own IPC "portals"
  (see [`06-service-layering.md`](06-service-layering.md)).
- `wlroots`: <https://gitlab.freedesktop.org/wlroots/wlroots>; the pinned version
  (`0.20.2`) supplies the standard protocol managers SDE uses.
