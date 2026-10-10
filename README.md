# Serenity Desktop Environment (SDE)

A project to recreate the **SerenityOS desktop experience** on Linux, built on **Wayland**.

The stack is split across a small family of repositories:

| Repository | Role |
|---|---|
| [`SerenityOS/serenity`](https://github.com/SerenityOS/serenity) | Upstream source of LibGUI / LibGfx / applications / themes / fonts. Consumed **unmodified** wherever possible. |
| [`jmalcolm137/libgui-wayland`](https://github.com/jmalcolm137/libgui-wayland) | **LibWindowServer** — runs unmodified Serenity `LibGUI`/`LibGfx` apps natively on Wayland by implementing the WindowServer protocol in-process (plus **LibSerenityAudio** for `LibAudio` ↔ PipeWire). |
| **this repo** | The **system side**: `WindowServer` (a wlroots-based Wayland compositor) and the session that runs the **unmodified SerenityOS shell apps** (Taskbar, applets, Desktop, Settings) as Wayland clients, plus services/settings and the integration that turns LibWindowServer-powered apps into a coherent desktop session. |

## Documents

- [`docs/01-objectives.md`](docs/01-objectives.md) — vision, goals, non-goals, principles, architecture, success criteria.
- [`docs/02-compositor-decision.md`](docs/02-compositor-decision.md) — options analysis for the compositor and the design decision.
- [`docs/03-engineering-conventions.md`](docs/03-engineering-conventions.md) — binding coding/build conventions (AK/LibCore/LibGfx, SerenityOS-style `serenity_main`, host compat in the SDE).

## Status

Implementation is underway. This repo contains:

- **`WindowServer/`** — the wlroots-based SDE Wayland compositor: `xdg-shell` and
  `layer-shell`, `xdg-decoration` negotiation, focus/stacking, compositor-drawn
  Serenity SSD frames, the compositor-side window menu (minimize / maximize /
  restore / move / roll up / menu bar / always on top / close), fullscreen, and
  the `serenity-window` menu-bar protocol.
- **`src/LibSerenitySystem/`** — host integration helpers.
- **`patches/`**, **`scripts/`** — SerenityOS host-build patches and the
  build / run / test scripts.

Today the unmodified SerenityOS **Taskbar** runs as a `layer-shell` panel and
LibGUI applications (TextEditor, …) run beside it with Serenity window chrome.
The session currently runs **nested** on a host Wayland compositor.

Milestone progress (see [`docs/01-objectives.md`](docs/01-objectives.md) §11):

| # | Milestone | Status |
|---|---|---|
| **R0** | Design | Done |
| **R1** | Compositor bring-up | In progress — nested dev session; real-hardware and headless bring-up outstanding |
| **R2** | Shell v1 | In progress — Taskbar runs; Desktop/Settings/wallpaper pending |
| **R3** | Serenity look & behaviours | Started — window chrome done; switcher/snap/workspaces pending |
| **R4** | Session & settings | Not started |
| **R5** | Hardening & packaging | Not started |

