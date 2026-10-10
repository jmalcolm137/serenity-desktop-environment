# Choice of Wayland Compositor — Options and Decision

**Status:** Draft v0.1 · **Date:** 2026-10-09
**Related:** [`01-objectives.md`](01-objectives.md)

---

## 0. TL;DR decision

**Build a dedicated Wayland compositor on top of `wlroots`, written in C++ and
aligned with the Serenity/AK stack. It is named `WindowServer`.**

- **Decorations:** server-side by strong preference, with **per-client
  negotiation** via `xdg-decoration` (a client that asks for CSD still gets it).
- **Shell = the real SerenityOS shell apps, as clients.** Run the unmodified
  `Userland/Services/Taskbar` panel (system-menu/start button, quick-launch,
  window list, applet area, clock, show-desktop) and its applets, Desktop and
  Settings as Wayland clients, mediated by the WindowServer serving the
  `WindowManagerServer` protocol, so the widgets and behaviour are SerenityOS's
  rather than a reimplementation. The
  compositor keeps the chrome SerenityOS implements *inside* `WindowServer`:
  server-side window frames, the window switcher, and snap/resize overlays.
- **Protocols:** expose standard `wlr-foreign-toplevel-management` for the wider
  ecosystem plus a small Serenity-specific window-management protocol for our
  own shell.
- **Rendering:** client rendering is CPU-bound — LibGUI draws through the CPU
  `Gfx::Painter` and there is no client GPU path — so `WindowServer`
  composites `wl_shm` surfaces with a GPU renderer for composition/scaling, and
  ships a **required Pixman software fallback** for headless/CI and no-GPU
  systems.

Use **KWin** only as an interim development host (LibWindowServer already targets it),
and use **River 0.4's compositor/WM split** as a *design influence and a
prototype harness*, not as the product base. Do **not** make KWin or River the
long-term foundation. A short spike (see §10) retires the main risks before the
commitment becomes large.

The rest of this document explains why, and what we would actually build.

---

## 1. Problem statement

The Serenity Desktop Environment needs a Wayland compositor that is
simultaneously:

1. a **correct, robust display server** (DRM/KMS, libinput, multi-output, HiDPI,
   session lock, idle, screenshots, XWayland);
2. a **window manager whose policy is Serenity's**, not i3's, GNOME's or KDE's
   (stacking, minimize/maximize/zoom, server-drawn frames, snapping,
   workspaces, an alt-tab switcher with live thumbnails);
3. a **platform that runs the real SerenityOS shell** — the unmodified Taskbar
   and its applets, the Desktop, notifications and Settings — which requires the
   Serenity `WindowManagerServer` protocol and Serenity window-type semantics
   (`Taskbar`, `Applet`, `AppletArea`, `Desktop`, `WindowSwitcher`, …); and
4. a foundation that lets us **reuse SerenityOS userland code** (AK, LibGfx,
   themes, fonts, and shell applications) rather than re-implementing the
   desktop in someone else's toolkit.

The compositor is where these four requirements collide, which is why it is the
first and largest architectural decision.

## 2. Where the boundaries must stay

Two facts from `libgui-wayland` constrain every option:

- **LibWindowServer is a client.** Each Serenity application is its own Wayland client,
  and LibWindowServer implements the WindowServer protocol *in-process*. Global duties are
  currently either stubbed or delegated to the host: window-management policy
  and server-side frames belong to the compositor, while the shell (taskbar,
  applets, wallpaper, switcher) is a set of clients.
- **Serenity's own architecture already separates concerns.** Inside
  SerenityOS's WindowServer there is a `Compositor`, a `WindowManager`, a
  `WindowFrame` (decorations), a `WindowSwitcher`, an `AppletManager`, an
  `Overlays` (snap previews), a `MenuManager`/`Menubar`, a `Screen`/
  `ScreenLayout`, and a `Cursor` — plus a separate **`WindowManagerServer` IPC
  protocol** that the Taskbar and switcher use.

That decomposition maps almost one-to-one onto a modern Wayland desktop:

| SerenityOS (inside WindowServer) | Wayland equivalent |
|---|---|
| `Compositor` | compositor's render/scanout path |
| `WindowManager` | window-management policy (tiling/stacking, focus, workspaces) |
| `WindowFrame` | server-side decorations / client-side decorations negotiation |
| `WindowSwitcher` | compositor-side switcher (the Serenity code lives in WindowServer) |
| `AppletManager` | compositor-side applet-area layout; each applet is a client window |
| `Overlays` | compositor-side snap/resize previews |
| `MenuManager`/`Menubar` | **already in LibWindowServer** (`WM::MenuRenderer`) |
| `WindowManagerServer` IPC | served by the WindowServer (compositor) directly |
| `Screen`/`ScreenLayout` | output management protocol |
| `Cursor` | cursor-shape / cursor theme + compositor pointer settings |

**Consequence:** the compositor's job is *global policy and plumbing*. The
app-like shell surfaces are clients; the chrome SerenityOS implements inside
`WindowServer` — window frames, the window switcher, snap/resize overlays — stays
with the compositor. The compositor owns window-management policy, Serenity
window-type semantics, and the privileged pieces clients cannot do for
themselves (input grabs, capture permission, layer ordering), and exposes
protocols for the rest.

**The shell is apps, not chrome.** SerenityOS's panel is a normal application:
`Userland/Services/Taskbar` creates a `WindowType::Taskbar` window and drives
`WindowManagerServer`. Its applets are separate `WindowType::Applet` windows laid
out by WindowServer's `AppletManager` into an `AppletArea`, with the Taskbar
supplying the position (`set_applet_area_position`) and receiving
`applet_area_rect_changed` / `WM_AppletAreaSizeChanged`. To get the *same widgets
and behaviour*, we reuse the Taskbar and applets **unmodified as clients** and
implement the server side — `WindowManagerServer`, Serenity window-type
semantics, and applet-area layout. This is the concrete meaning of "the rest of
the shell is clients"; only the switcher/overlays/frames stay compositor-side,
because that is where SerenityOS keeps them. The applet set is the one from
`Base/etc/WindowServer.ini` (`WorkspacePicker, CPUGraph, MemoryGraph,
NetworkGraph, Network, ClipboardHistory, Audio, Keymap`).

## 3. Evaluation criteria

Weights are indicative; licensing and stack fit are treated as near-hard
constraints, and the raw score is a sanity check rather than the decision.

| # | Criterion | Weight | What it means |
|---|---|---|---|
| C1 | **Control & fidelity** | 20 | How much of the Serenity experience can we realise without fighting the compositor? |
| C2 | **Serenity / C++ stack fit** | 15 | Can we link AK/LibGfx/LibGUI and reuse Serenity code? |
| C3 | **Protocol coverage** | 15 | layer-shell, window management, output management, capture, lock/idle, XWayland. |
| C4 | **Maturity & risk** | 15 | Production readiness, maintainers, API stability, GPU breadth. |
| C5 | **Effort to a usable session** | 15 | Time for a small team to a daily-driver session. |
| C6 | **Extensibility** | 10 | Adding Serenity-specific protocols/overlays. |
| C7 | **License compatibility (keep BSD-2)** | 5 | Can we link deps and avoid GPL obligations? |
| C8 | **Long-term maintenance** | 5 | Who maintains the foundation? |

## 4. Options

### Option A — KWin (KDE Plasma's compositor)

Run KWin as the session compositor and provide a Serenity shell on top.

- **Pros:** the most mature, complete Linux compositor; excellent
  DRM/KMS, libinput, multi-GPU, HiDPI/fractional-scale and XWayland; supports
  `wlr-layer-shell` (so a taskbar/applets can be ordinary layer-shell clients);
  window rules, scripts (JS/QML), virtual desktops, snapping, screenshot and
  recording infrastructure; used with third-party shells already (e.g. Noctalia,
  "KineticWE"), so "KWin without Plasma" is a supported shape; least compositor
  code for us to own.
- **Cons:** KWin does **not** implement `wlr-foreign-toplevel-management`;
  window management is exposed through KDE-specific protocols
  (`org_kde_plasma_window_management`, `kde-plasma-shell`), so our
  `WindowManagerServer` needs a KDE backend and the shell is pinned to KDE.
  Customisation lives in Qt/QML/JS, not our AK/LibGUI world, so Serenity chrome
  (window frames via Aurorae/QML, a Serenity alt-tab) is an *approximation* with
  a real fidelity ceiling. It is a large, independently-developed system whose
  semantics (tiling modes, activities, rules, KConfig) we would spend time
  bending. Extending it deeply means writing GPL plugins; embedding our
  BSD-2/LibGUI components inside it is not practical.
- **Verdict:** the strongest choice for *shipping something quickly*, the
  weakest for *owning and reproducing Serenity*. Excellent interim host.

### Option B — River 0.4 (separate compositor and window manager)

River is a wlroots-based compositor that, since 0.4.0 (2026), deliberately
**splits the compositor from the window manager**: the compositor provides
display, rendering, input and low-level plumbing; a separate WM program drives
policy over the stable `river-window-management-v1` protocol (window position,
keybindings, focus, server-side decorations). River itself advertises robust
XWayland support and supports many protocol extensions.

- **Pros:** directly embodies the compositor/WM separation the Serenity
  architecture already has, and lets us write the "WindowManager" — the part
  that must be Serenity-shaped — as an ordinary, restartable program (in C++,
  linked against AK), while River handles frame-perfect rendering and the hard
  plumbing. A WM crash does not lose the session; WMs are hot-swappable. Stable
  protocol with a growing catalogue of WMs. Far less work than writing a
  compositor.
- **Cons:** River 0.4 defers *all* window-management policy to the WM — which is
  exactly the layer we would write — so it saves us the compositor but not the
  window management; and the reference/ecosystem WMs are tiling-first (river
  classic was a tiling compositor), so we would be implementing Serenity's
  stacking semantics in a young protocol field. River 0.4 is **new** (2026),
  small-team/single-maintainer, and explicitly out of scope for visual effects
  or custom shaders "for a year or two", which caps how far we can go on
  switcher/overlay rendering. Shell protocols such as `layer-shell` are
  **mediated through the WM** (`river-layer-shell-v1`), adding an unusual
  indirection. It is **GPL-3.0-only**: fine as a separate process — our WM is a
  protocol client and River's Wayland *protocols* are MIT — but River itself
  cannot be linked into BSD-2 code. Writing a compositor is replaced by writing
  a compositor *protocol client* against a young API — a different risk, not no
  risk.
- **Verdict:** an elegant and genuinely interesting fit for the *window-manager*
  half. Best used now as a **prototype harness** to validate our
  `WindowManagerServer` mapping against a real separated WM. Not recommended as
  the product base because it is young, pushes the whole window management
  problem (plus shell-protocol mediation) onto us anyway, and cannot be linked
  into our BSD-2 code.

### Option C — `WindowServer` on `wlroots` (C++)  ← recommended

Write our own compositor (`WindowServer`) against the `wlroots` library
(MIT), in C++, starting from a minimal example and growing it into the product
compositor.

- **Pros:** maximum control over policy, protocols and rendering; we can draw
  Serenity window frames and overlays directly with LibGfx/theme code, and can
  define a Serenity-shaped window-management protocol alongside the standard
  `wlr-foreign-toplevel-management`. `wlroots` supplies the expensive, undying
  parts: DRM/KMS backends, libinput handling, output/scene graph, xdg-shell,
  layer-shell, xdg-decoration, foreign-toplevel, output-management, screencopy/
  image-capture, idle, session-lock, primary-selection, and XWayland. MIT
  licence keeps our BSD-2 position; C is trivially consumable from C++ and can
  coexist with AK/LibGfx in the same process for server-side window frames. A
  GPU renderer handles composition/scaling while a Pixman fallback covers
  headless/CI. We own exactly the layer that defines the experience (P5).
- **Cons:** the most work of any option: we implement focus/stacking/workspaces/
  snapping/output config/input config/session behaviour ourselves. Reaching the
  robustness of KWin across GPUs and fractional scaling takes time. We must
  track `wlroots` API evolution. We become maintainers of a compositor.
- **Verdict:** the only option that fully satisfies C1, C2, C6 and C7 at once.
  Effort is the price; it is reduced by borrowing proven code paths (wlroots
  scene/protocol helpers) and by a staged build.

### Option D — Fork/extend an existing wlroots compositor

Take an existing wlroots compositor and add Serenity behaviour to it.

- **Candidates:** **labwc** (lightweight, Openbox-inspired, *stacking* — the
  closest match to Serenity's model, GPL-2.0); **Wayfire** (plugin-oriented,
  MIT, but Compiz-like); **Sway** (i3-like tiling — poor match, MIT).
- **Pros:** a functioning compositor and WM on day one; labwc's stacking model
  is close to Serenity, so much of the window management is already there;
  Wayfire's plugin system is a legitimate extension point and is MIT.
- **Cons:** we inherit another project's configuration, design and release
  cadence, and must either carry a fork (divergence, rebases) or work through
  limited extension points. labwc's GPL-2.0 would force our compositor to be
  GPL if we fork it — breaking our BSD-2 goal. The shell work (taskbar,
  switcher, theme) is identical to Option C, so the saving is only in the
  compositor core.
- **Verdict:** the best *fallback* and a good source of reference code, but not
  the cleanest home for Serenity policy if we want both control and a
  permissive licence.

### Option E — `libweston`

Use libweston, whose explicit purpose is building custom environments with a
replaceable shell.

- **Pros:** designed exactly for "custom shell on a library"; stable; used in
  embedded/automotive; permissive.
- **Cons:** a smaller and less modern desktop protocol ecosystem than wlroots;
  fewer compositors to borrow from; rendering/capture/layer-shell support lags;
  less community momentum for a desktop use case.
- **Verdict:** a reasonable embedded choice, not the best desktop one.

### Option F — Smithay (Rust)

Build the compositor in Rust on Smithay (MIT), as COSMIC and niri do.

- **Pros:** modern, safe, active; excellent protocol coverage including wlroots
  protocols; strong precedent for full desktops.
- **Cons:** the shell would normally be the same language/Rust ecosystem; our
  differentiated code is C++/AK/LibGUI, so we would either FFI into Rust (heavy,
  against P3) or split the compositor and shell across two worlds. Adds a second
  toolchain and community for no fidelity gain.
- **Verdict:** technically viable because the shell can be a separate process,
  but a poor fit for a Serenity/AK codebase. Rejected.

### Option G — From scratch on `libwayland-server`

Write the display server, input and rendering without a compositor library.

- **Pros:** total control; no dependency.
- **Cons:** re-implements DRM/KMS, libinput, xdg-shell, layer-shell, output
  management, capture, XWayland — tens of thousands of lines that `wlroots`
  exists to provide. Not rational for this project.
- **Verdict:** rejected. `wlroots` is, as its own tagline admits, "the 60k lines
  of code you were going to write anyway."

## 5. Comparison matrix

Scores are 1 (poor) to 5 (excellent); weighted out of 5.00.

| Criterion (weight) | A KWin | B River | C wlroots (C++) | D Fork wlroots | E libweston | F Smithay | G Scratch |
|---|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| C1 Control & fidelity (20%) | 2 | 3 | **5** | 3 | 3 | 3 | 5 |
| C2 Serenity/C++ fit (15%) | 2 | 4 | **5** | 3 | 3 | 2 | 4 |
| C3 Protocol coverage (15%) | 4 | 4 | 4 | 4 | 2 | 4 | 3 |
| C4 Maturity & risk (15%) | 5 | 3 | 2 | 4 | 4 | 4 | 1 |
| C5 Effort to usable (15%) | 3 | **4** | 2 | **4** | 2 | 2 | 1 |
| C6 Extensibility (10%) | 2 | 3 | **5** | 3 | 3 | 4 | 5 |
| C7 License / BSD-2 (5%) | 2 | 3 | **5** | 2* | 5 | 5 | 5 |
| C8 Maintenance (5%) | 5 | 3 | 3 | 4 | 3 | 4 | 1 |
| **Weighted total** | **3.05** | **3.45** | **3.85** | **3.50** | 2.95 | 3.25 | 3.15 |

\* A labwc fork would be GPL-2.0 (score 2); a Wayfire fork (MIT) would score 5
here but lower on C1. River is GPL-3.0-only but its protocols are MIT and it is
used as a separate process, hence 3.

The score confirms what the qualitative analysis already shows: the two serious
finalists are **Option C** (own it on wlroots) and **Option D** (extend a
wlroots compositor). River is a close third and is uniquely valuable as a
prototyping harness. KWin is the best *interim* choice and the worst *product*
choice.

## 6. Analysis: the decisive axes

**Control and fidelity (C1, C6).** The thing that makes the environment
"Serenity" is the shell and the window-management semantics, and both are things
we must own. KWin's window-management policy is KDE's; bending it to Serenity's
stacking/zoom/snap/applet model is a fight, and its rendering of chrome lives in
QML/Aurorae, not LibGfx. River hands us window management policy but keeps
rendering and shell protocols at one remove. Only owning the compositor gives us
Serenity frames, a Serenity switcher with live thumbnails, and a
Serenity-shaped management protocol, all drawn with the same theme code the apps
use. This is the strongest argument for Option C.

**Stack fit (C2).** Our differentiated code is C++/AK/LibGUI. A C++ compositor
on wlroots (C) lets server-side window frames share LibGfx and the theme; it also
lets us keep one build system (Lagom) and one set of conventions. Qt/JS (KWin)
and Rust (Smithay) each create a second world.

**Licensing (C7).** We intend to stay BSD-2, matching Serenity and LibWindowServer.
`wlroots` is MIT, `libweston` is MIT, `Smithay` is MIT, `Wayfire` is MIT — but
**labwc is GPL-2.0** (a fork forces GPL), **River is GPL-3.0-only** (usable only
across a process/protocol boundary; its protocols are MIT), and a deep **KWin**
extension is GPL. This quietly eliminates "fork labwc" and "write a KWin
plugin" for a project that wants permissive code, and is a tiebreaker in favour
of Option C.

**Effort (C5).** This is the counter-argument, and it is real: Option C is the
most work. It is mitigated three ways. First, `wlroots` supplies the hard,
undying parts. Second, the shell (the bulk of the visible work) is
protocol-portable, so it can be built and tested against KWin or a headless
compositor *before* our compositor is ready. Third, the compositor can be
grown from a minimal core, adding one responsibility at a time, exactly as
LibWindowServer was grown milestone by milestone. The risk is bounded by a spike.

**Maturity (C4).** KWin wins this outright, which is why it stays as the dev
host. But "mature" is not the same as "fits": no mature compositor implements
Serenity's semantics, so maturity buys robustness of plumbing, not of
experience. Our mitigation is to rent the plumbing from wlroots (which *is*
mature in exactly those areas) and own only the policy layer.

**Fault isolation (N4).** River's separated WM is attractive here: a WM crash
does not lose the session. We can get much of that benefit without River by
keeping the *shell* (taskbar, applets, wallpaper) as ordinary clients that can
crash and restart independently, and by keeping the compositor's policy code
small and well-tested. We do not need to split the compositor itself to get most
of the robustness.

### Rendering model: CPU clients, GPU compositor

Inspecting LibGUI settles the GPU question. `GUI::Painter` is a thin subclass of
`Gfx::Painter`, which is a **CPU scanline rasteriser** that draws into a
`Gfx::Bitmap`; `LibGUI` links `LibGfx`/`LibCore`/… but **not** `LibGPU` or
`LibGL`, and `LibGfx` does not either. Serenity's client rendering path is
therefore software by construction, and it stays that way: an application
presents a CPU-rendered `BGRA8888`/`BGRx8888` `wl_shm` buffer — exactly what
LibWindowServer already forwards.

So GPU/OpenGL is **not** a whole-project non-goal; it is simply located at the
compositor, not the client:

- `WindowServer` should composite with a GPU renderer (`wlroots`' GLES2
  or Vulkan renderer) so that many `wl_shm` surfaces, HiDPI scaling and
  animation stay affordable; each surface is uploaded once per damage.
- A **software (Pixman) renderer** is a required fallback: headless CI, VMs,
  systems without a usable GPU, and the early bring-up phase.
- The compositor's own chrome — server-side window frames, the window switcher
  and overlays — is drawn with LibGfx into a bitmap and uploaded as a texture.
  Frames are small and change rarely, so CPU drawing is not a bottleneck.
- **Crisp HiDPI is a client-rendering problem, not a GPU problem.** The fix is to
  plumb the output's device scale into `LibGUI`'s backing store (the existing
  `Window::create_backing_store` FIXME / LibWindowServer M8) so apps rasterise at native
  resolution and the compositor presents 1:1; the renderer does fractional-scale
  filtering only when it must.
- `LibGPU`/`LibSoftGPU`/`LibGL` remain out of scope *for the desktop shell*;
  they matter only if an application (e.g. WebGL in the browser) chooses to use
  them, and that still surfaces through LibGUI as a bitmap.

## 7. Decision

**Adopt Option C: build `WindowServer`, a dedicated Wayland compositor
on `wlroots` (MIT) in C++, integrated with the AK/LibGfx stack.**

Specifically:

1. **Compositor core** — `WindowServer`, `wlroots` (MIT), C++23, Serenity
   code style, built with Lagom. Started from a minimal example, grown
   responsibility by responsibility.
2. **Window management policy** is ours: stacking, focus, minimize/maximize/
   zoom, snap, workspaces, fullscreen — Serenity semantics.
3. **Decorations: server-side by preference, with per-client negotiation.**
   `WindowServer` advertises `xdg-decoration` in server-side mode and
   draws Serenity window frames with LibGfx/theme, so all toplevels (Serenity and
   native) get Serenity chrome. A client that explicitly requests client-side
   decorations still gets them; LibWindowServer requests SSD and retains its CSD path only
   for compositors that cannot provide SSD (e.g. some dev hosts).
4. **Renderer split.** The compositor composites `wl_shm` client buffers with a
   GPU renderer (GLES2/Vulkan), with a **required Pixman software fallback** for
   headless/CI and no-GPU systems. Client rendering stays CPU (LibGUI); the
   compositor never requires clients to render on the GPU.
5. **Shell = unmodified SerenityOS shell apps.** Run `Userland/Services/Taskbar`
   (system-menu/start button, quick-launch, window list, applet area, clock,
   show-desktop) and its applets, the Desktop and Settings as Wayland clients.
   `LibWindowServer` maps Serenity **window types** directly onto roles: the
   `Taskbar`, `Applet` and `Desktop` become `wlr-layer-shell` surfaces (a bottom
   panel with an exclusive zone, a top-layer applet, a background desktop), while
   the remaining types are ordinary `xdg-shell` toplevels with compositor-drawn
   frames. No `app_id` role hint is involved, so the widgets land correctly
   without modification.
6. **`WindowManagerServer` is served by the WindowServer.** Just as in
   SerenityOS, the WindowServer hosts the shell's `WindowManagerServer` protocol:
   the compositor listens on `/tmp/portal/wm`, answers the Serenity
   `WindowManagerServer` requests (`set_active_window`, `set_window_minimized`,
   `toggle_show_desktop`, `start_window_resize`, `popup_window_menu`,
   `set_window_taskbar_rect`, `set_applet_area_position`, `set_workspace`,
   `set_keymap`) and emits the `WindowManagerClient` events
   (`WM_WindowStateChanged`, `WM_WindowRemoved`, …, `WM_AppletAreaSizeChanged`)
   straight from its own window list. No bridging shim or bespoke protocol is
   involved. Also expose standard `wlr-foreign-toplevel-management` for
   ecosystem tools.
7. **Compositor chrome stays compositor-side.** Window frames, the window
   switcher and snap/resize overlays are ported from the corresponding
   `WindowServer` components (`WindowFrame`, `WindowSwitcher`, `Overlays`),
   because SerenityOS implements them there and exact reuse means keeping them
   with the compositor. The chrome is fully interactive: titlebar buttons
   hover/press and act on release, double-clicking the titlebar toggles
   maximize, resize borders show resize cursors, and right-clicking the titlebar
   (or clicking the window icon) opens the compositor-rendered window menu
   (Minimize/Maximize/Move/Roll Up/Always on Top/Close).
8. **Session plumbing** — lock, idle, output management, capture, XWayland,
   input config — from wlroots/standard protocols, with per-compositor
   backends only where no standard exists (mirroring LibWindowServer's existing
   `DESIGN.md` policy).

**KWin remains a supported development host.** LibWindowServer already targets it; the
Taskbar and shell apps are ordinary clients, so they can run against KWin via a
KDE window-management backend (the settings backend interface already anticipates
one). Nothing about this decision prevents an SDE session from running on KWin
during bring-up or as a fallback.

**River is adopted as an influence and a spike, not the base.** Its
compositor/WM split validates the `WindowManagerServer` design; a short spike (§10)
runs our WindowManagerServer mapping against a real separated WM to retire protocol risk
before we invest in the compositor core.

## 8. Target architecture

```
                        ┌────────────────────────────────────────────┐
                        │ Unmodified Serenity apps   native apps     │
                        │        │ LibWindowServer (client)     │              │
                        └────────┼────────────────────┼──────────────┘
                                 │ xdg-shell, wl_shm, │
                                 │ xdg-decoration(SSD)│
   ┌─────────────────────────────▼────────────────────▼───────────────┐
   │                   WindowServer  (wlroots)                 │
   │                                                                  │
   │  display server   │  window management   │  WindowServer chrome  │
   │  (wlroots: DRM,   │  (stacking, focus,   │  · SSD frames         │
   │  libinput, output)│  workspaces, snap)   │  · switcher · overlays│
   │                                                                  │
   │  renderer: GPU (GLES2/Vulkan) with Pixman software fallback       │
   │                                                                  │
   │  protocols: xdg-shell · layer-shell · xdg-decoration ·           │
   │             foreign-toplevel · output-management · capture ·     │
   │             idle · session-lock · XWayland      │
   └───────▲───────────────────────────────────────────────▲──────────┘
           │ layer-shell + foreign-toplevel / WindowManagerServer  │
   ┌───────┴───────────────────────────────────────────────┴──────────┐
   │ SerenityOS shell apps (unmodified, LibGUI/LibGfx clients)        │
   │   Taskbar(system menu·quick-launch·window list·clock) · Applets  │
   │   Desktop · Settings · Notifications · Wallpaper                 │
   │   ⇅ WindowManagerServer (served by the WindowServer)               │
   └──────────────────────────────────────────────────────────────────┘
```

Responsibilities in one line each:

- **`WindowServer`:** owns displays, input, stacking, window-type
  semantics, the window-management protocols, and the WindowServer chrome it
  inherits (frames, switcher, overlays).
- **Shell clients:** the unmodified SerenityOS Taskbar, applets, Desktop,
  Settings and notifications. Ordinary clients; can crash/restart.
- **LibWindowServer:** owns the per-application WindowServer protocol (unchanged
  philosophy); requests SSD, falls back to CSD when the compositor cannot.
- **WindowManagerServer:** served by the WindowServer itself (`/tmp/portal/wm`),
  driven directly from the compositor's window list.

## 9. Why not the alternatives (summary)

| Option | One-line reason rejected as the base |
|---|---|
| **KWin** | Mature plumbing but KDE policy/protocols and QML chrome cap Serenity fidelity; no `wlr-foreign-toplevel`; GPL deep extension. Kept as dev host. |
| **River** | Elegant split, but young (0.4, 2026), single-maintainer, pushes window management and shell-protocol mediation onto us, and is GPL-3.0-only. Kept as a prototype harness. |
| **Fork labwc/Wayfire/Sway** | Inherit a foreign design and (labwc) GPL; shell work is unchanged, so little is actually saved. Kept as fallback/reference. |
| **libweston** | Purpose-built for custom shells but a weaker desktop protocol ecosystem. |
| **Smithay** | Second language/Rust world, no fidelity or reuse gain over C++/AK. |
| **From scratch** | Re-implements what wlroots already provides; not rational. |

## 10. De-risking plan (staged)

The decision is a direction, not a big-bang commitment. Each phase has an exit
criterion and a fallback.

**Phase 0 — spike (days), on existing infrastructure.**
- Run the unmodified Taskbar against a `WindowManagerServer` talking to a
  **River** WM (or KWin's `org_kde_plasma_window_management`) and confirm the
  enumeration/activation/minimize/close model maps.
- Stand up `tinywl`-style wlroots compositor and run one LibWindowServer app plus a
  `layer-shell` panel.
- **Exit:** WMServer mapping validated; wlroots builds and presents LibWindowServer
  buffers. **Fallback:** if wlroots integration is blocked, escalate to
  labwc/Wayfire before writing more.

**Phase 1 — compositor core.**
DRM/libinput/outputs, xdg-shell, layer-shell, xdg-decoration, focus/stacking,
one SSD Serenity frame, headless backend. **Exit:** run Calculator + a native
app + a panel, with Serenity frames, on real and headless outputs.

**Phase 2 — window management + shell.**
Workspaces, minimize/maximize/zoom, snapping; `WindowManagerServer` and
window-type semantics; then adopt the **unmodified SerenityOS Taskbar** and its
applets, plus wallpaper/Desktop. **Exit:** daily-driveable Serenity desktop for
LibGUI apps.

**Phase 3 — chrome and overlays.**
Switcher with live thumbnails (`ext-image-copy-capture`), snap overlays,
applets/system tray, notifications, theming/font parity. **Exit:** structural
match to Serenity screenshots.

**Phase 4 — session and settings.**
Login/autostart/lock/power; display/input/appearance/cursor backends;
Terminal + SystemMonitor; HiDPI (close LibWindowServer M8); XWayland. **Exit:** a complete
session.

**Phase 5 — hardening.**
Multi-GPU/multi-output, fractional scaling, packaging, CI matrix, docs.

If, at the end of Phase 1, owning the compositor is proving disproportionate to
the team's capacity, the contract with the shell lets us fall back to
Option D/labwc or to KWin with a KDE backend and lose little of the shell work.

## 11. Consequences and trade-offs accepted

- **We take on compositor maintenance.** Accepted in exchange for fidelity,
  licence and control. Bounded by wlroots and by staging.
- **Longer path to a daily driver** than simply adopting KWin. Mitigated by
  keeping KWin as the dev host and the shell protocol-portable.
- **A custom window-management protocol** to maintain, alongside the standard
  `wlr-foreign-toplevel-management`. Kept minimal and stable.
- **CPU client rendering is fixed.** LibGUI draws with the CPU `Gfx::Painter`
  and presents `wl_shm`; `WindowServer` treats that as the primary path
  and keeps a Pixman software renderer as a first-class fallback. GPU
  acceleration lives only in composition/scanout.
- **Server-side decorations by default, with negotiation.** Serenity apps look
  native; native apps get Serenity frames unless they request CSD, which is
  intentional for a coherent desktop.
- **Shell parity requires protocol fidelity, not new widgets.** The Taskbar and
  applets run unmodified, so `WindowManagerServer`, window-type semantics and
  applet-area layout must match SerenityOS closely. This is real work, but it is
  bounded and testable directly against the real apps.
- **Size-based font zoom needs a vector font.** The default UI family is
  Katica, which upstream ships only as bitmap faces (sizes 10 and 12), as it
  does for the fixed-width Csilla family. `Widget::set_font_size()` calls
  `FontDatabase::get(..., AllowInexactSizeMatch::No)`, so requesting any other
  size resolves to nothing and the widget falls back to its default size. The
  TextEditor Zoom In/Out actions (`Ctrl+=` / `Ctrl+-`) therefore do nothing with
  the stock font — matching upstream SerenityOS, which also defaults to Katica.
  Arbitrary sizes (and thus working zoom) need a scalable face such as the
  bundled `SerenitySans-Regular.ttf`; selecting system fonts is part of the
  planned settings work (objective **G5**). See
  [`10-font-parity-decision.md`](10-font-parity-decision.md) for the R3 decision
  and the R4/G5 plan.
- **The window switcher and overlays are compositor chrome**, matching where
  SerenityOS implements them (`WindowServer`), rather than shell clients.
- **XWayland is a compositor feature**, not a shell one; supported where
  wlroots supports it.

## 12. Open questions

1. **`WindowManagerServer` coverage:** which Serenity `WindowManagerServer`
   semantics the compositor must serve itself beyond window state/removal
   (workspace IDs? applet-area geometry?). Workspaces and the applet area are
   still no-ops: applets are layer surfaces positioned by the compositor, and
   the window menu is rendered compositor-side.
2. **Frame rendering path:** LibGfx into an uploaded texture versus a dedicated
   wlroots renderer pass for the SSD frame; decide in Phase 1.
3. **Session integration:** logind/DRM master, seat management, and the login
   manager to target first.
4. **Capture permission model** for the switcher's thumbnails: a compositor
   policy/portal so a trusted shell client can capture toplevels.
5. **Switcher/overlay protocol details** — whether the compositor carries a
   ready-made thumbnail or the client uses `ext-image-copy-capture` directly.
6. **Packaging** of `WindowServer` and the session (binary/session names,
   install layout).
7. **Window switcher placement:** confirm compositor-side (Serenity-faithful,
   our recommendation, since `WindowSwitcher` is WindowServer code) versus a
   capture-based shell client.

## 13. Appendix — protocol inventory (target)

| Concern | Protocol(s) |
|---|---|
| Surface roles | `xdg-shell`, `wlr-layer-shell-unstable-v1` |
| Decorations | `xdg-decoration-unstable-v1` (SSD) |
| Window types | Serenity `WindowType` semantics. `LibWindowServer` maps the shell types (`Taskbar`, `Applet`, `Desktop`) directly onto `wlr-layer-shell` roles; the rest are `xdg-shell` toplevels with compositor-drawn frames (no `app_id` hint) |
| Shell session (WMServer) | `WindowManagerServer` requests + `WindowManagerClient` events (windows, workspaces, applet area, super-key, quick-launch) |
| Window management (shell) | `wlr-foreign-toplevel-management-unstable-v1` |
| Outputs / config | `wl_output`, `zxdg_output_v1`, `wlr-output-management-unstable-v1` |
| Capture (switcher/screenshots) | `ext-image-copy-capture-v1` / `wlr-screencopy-unstable-v1` (+ portal) |
| Input | `wl_seat`, `wl_pointer`, `wl_keyboard`, `wl_touch`, `wlr-virtual-pointer`, `wlr-virtual-keyboard` |
| Clipboard / DnD | `wl_data_device`, `wlr-data-control` / `ext-data-control` |
| Idle / lock | `ext-idle-notify-v1`, `ext-session-lock-v1` |
| Cursors | `cursor-shape-v1`, `wlr-cursor-shape`, theme via portal |
| Shortcuts | `org.freedesktop.portal.GlobalShortcuts`, `wlr-foreign-toplevel` activation |
| X11 apps | XWayland (wlroots) |
| Settings / appearance | `org.freedesktop.portal.Settings` + per-compositor backend |

For the live support status of each protocol above — and to compare our target
inventory against what wlroots-based compositors (sway, Hyprland, labwc, …) and
others actually expose — see the community **Wayland protocols support table**
in §14.

## 14. References

- Wayland protocol index — <https://wayland.app/protocols/>
- **Wayland protocols support table** — a community cross-compositor matrix (which
  compositors implement which protocol/interface, with descriptions and per-compositor
  support percentages), generated from `wayland-explorer`/`wayland.app` data. Use it to
  sanity-check our protocol inventory and see how wlroots-based compositors compare:
  <https://absurdlysuspicious.github.io/wayland-protocols-table/>
  (source: <https://github.com/AbsurdlySuspicious/wayland-protocols-table>).
- `wlroots` — <https://gitlab.freedesktop.org/wlroots/wlroots> (MIT)
- KWin — <https://github.com/KDE/kwin>; `kde-plasma-shell`, `org_kde_plasma_window_management`; `wlr-layer-shell` support noted by third-party shell projects.
- River 0.4 / `river-window-management-v1` — "Separating the Wayland Compositor and Window Manager", Isaac Freund, 2026-03-15 — <https://isaacfreund.com/blog/river-window-management/>; FOSDEM 2026 talk. River is GPL-3.0-only; its Wayland protocols are MIT.
- River wiki, compatible window managers and `tinyrwm` reference WM — <https://codeberg.org/river/wiki>.
- labwc — <https://github.com/labwc/labwc> (GPL-2.0); Wayfire — <https://wayfire.org/> (MIT).
- libweston — Weston documentation, "Libweston" — <https://wayland.pages.freedesktop.org/weston/>.
- Smithay — <https://github.com/Smithay/smithay> (MIT).
- Rendering model — `Userland/Libraries/LibGUI/Painter.h` (subclass of `Gfx::Painter`); `Userland/Libraries/LibGfx/Painter.{h,cpp}` (CPU scanline rasteriser); `LibGfx/CMakeLists.txt` and `LibGUI/CMakeLists.txt` (no `LibGPU`/`LibGL` dependency); `Userland/Libraries/LibGPU/` (GPU abstraction used by `LibGL`/`LibSoftGPU`, not LibGUI).
- Shell — `Userland/Services/Taskbar/{main,TaskbarWindow,TaskbarButton,QuickLaunchWidget,ClockWidget,ShutdownDialog}.cpp`; `Userland/Services/WindowServer/AppletManager.cpp`; `Userland/Services/WindowServer/WindowManagerServer.ipc`; `Base/etc/WindowServer.ini` (`[Applet] Order=…`); `Userland/Services/WindowServer/WindowType.h` (`LibGUI/WindowType.h` re-exports it).
- `libgui-wayland` / LibWindowServer `DESIGN.md` — <https://github.com/jmalcolm137/libgui-wayland/blob/main/DESIGN.md> (per-compositor backend policy, WindowServer protocol disposition).
- Service layering (which Serenity service belongs in `libgui-wayland` vs. the SDE session; the "prefer the session service, else in-process fallback" rule) — [`06-service-layering.md`](06-service-layering.md).

---

## 15. Addendum — Louvre (C++ compositor library)

Evaluated after implementation started. **Louvre** (<https://github.com/CuarzoSoftware/Louvre>) is a
C++20 library for building Wayland compositors, developed by Cuarzo Software (v3.0.0, 2026). It
uses a factory-method design: it ships default implementations of the major protocols and lets
the compositor override them incrementally, so a working compositor is fast to stand up.

**What it offers.** Backends: DRM/KMS, Wayland (nested), Offscreen. Renderers: OpenGL, Raster,
Vulkan (WIP). Protocols include `xdg-shell`, `wlr-layer-shell`, `wlr-foreign-toplevel-management`,
`ext-foreign-toplevel-list`, `xdg-decoration`, `wlr-output-management`, `session-lock`, idle
notify/inhibit, fractional-scale, viewporter, pointer constraints/gestures, `xdg-activation`,
gamma control, and more. Features: explicit synchronization, fractional scaling, multi-GPU,
multi-session, double/triple buffering, persistent clipboard, rootful XWayland.

**Pros for the SDE.**
- Native C++ API: no `extern "C"` wrappers and no header patches (wlroots' headers are C-only; we
  had to add `extern "C"` and normalize `[static N]` array parameters and a `namespace` member).
- Higher-level scene/input handling and sensible defaults — less compositor code to own.
- Covers the shell-relevant protocols (layer-shell, foreign-toplevel, decoration, output
  management, session lock, idle, fractional scale).

**Cons for the SDE.**
- **LGPL-2.1** (weak copyleft), versus wlroots' **MIT**. Usable from a BSD-2 project when
  dynamically linked and unmodified, but it adds relink/source obligations our decision
  deliberately avoided.
- Smaller ecosystem (a single maintainer, ~700 stars) versus wlroots' maturity (sway, Hyprland,
  labwc, gamescope, …) and much larger test surface.
- **Rootful XWayland only** (no rootless), versus wlroots' rootless XWayland (seamless X11 apps).
- Capture for the window switcher is weaker: `ext-image-capture-source` exists but image copy
  capture / screencopy is WIP, and `wlr-data-control` and `wlr-virtual-pointer`/`-keyboard`
  (useful for clipboard managers and headless input injection) are absent.
- Its own renderer/scene graph differs from our planned LibGfx/LibGPU chrome path (true of
  wlroots too, but wlroots' C renderer boundaries are already proven for us here).

**Verdict.** Louvre is a genuinely good fit for a *C++* compositor and would remove the wlroots C
boundary almost entirely. But for the SDE's stated priorities — permissive licensing, mature
protocol coverage (rootless XWayland, capture, virtual input, data-control), and a large
ecosystem — **wlroots remains the better foundation**. The wlroots C friction is now bounded to
a small, local wrapper (`extern "C"` plus a documented header normalization), and we should hide
wlroots behind our own C++ "platform" layer so `WindowServer`'s policy code stays
Serenity-style C++/AK. Keep Louvre as a documented alternative and, if desired, run a time-boxed
spike (nested compositor + Calculator + a layer-shell panel) before any future re-evaluation.

Note: the Phase 0 spike was completed on wlroots — the nested `WindowServer` created an
output and ran the unmodified SerenityOS `Calculator` (mapped as a toplevel, frames presented).

