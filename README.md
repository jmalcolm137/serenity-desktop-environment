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

Draft. Design phase — no implementation in this repo yet.
