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
- [`docs/04-known-gaps.md`](docs/04-known-gaps.md) — running list of known gaps and remaining work, so nothing found during bring-up is forgotten.

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
| **R1** | Compositor bring-up | Done — nested, headless (CI-tested) and DRM/libinput session paths |
| **R2** | Shell v1 | Done — Taskbar, WindowManagerServer, wallpaper, Desktop, file icons, Launch |
| **R3** | Serenity look & behaviours | Started — window chrome done; switcher/snap/workspaces pending |
| **R4** | Session & settings | Not started |
| **R5** | Hardening & packaging | Not started |

## Running

Build the compositor (links the `libgui-wayland` userland libraries and the
pinned `wlroots`):

```sh
scripts/build-window-server.sh
```

**Nested (development).** Run the compositor as a Wayland client of your host
session; it hosts the shell and Serenity apps:

```sh
SERENITY=/path/to/libgui-wayland/serenity \
BUILD=/path/to/libgui-wayland/build/lagom \
WAYLAND_DISPLAY=wayland-0 SERENITY_RES_ROOT="$SERENITY/Base/res" \
LD_LIBRARY_PATH="$BUILD/lib" \
build/WindowServer/WindowServer -n sde \
  -s "$BUILD/bin/Taskbar & $BUILD/bin/FileManager -d & $BUILD/bin/TextEditor"
```

(`FileManager -d` is the SerenityOS Desktop — icons over the wallpaper.)

**Headless (CI).** `scripts/run-headless-test.sh` runs the compositor with the
headless backend and asserts that it hosts a LibWindowServer app, a layer-shell
panel and a native Wayland app — the R1 exit criterion, with no display needed.

**Real hardware (DRM/KMS + libinput).** From a TTY (`Ctrl+Alt+F3`) or a display
manager session entry:

```sh
scripts/start-sde-session.sh
```

With `WAYLAND_DISPLAY`/`DISPLAY` unset, `wlr_backend_autocreate` selects the
DRM+libinput backend and `libseat` grants DRM master. The launcher starts the
compositor on socket `sde`, serves the WindowManagerServer on `/tmp/portal/wm`,
and starts the Taskbar and a TextEditor.

**Desktop background.** The compositor paints the theme's `DesktopBackground`
colour behind everything (SerenityOS's default). Set `SDE_WALLPAPER` to an
absolute path, or to a name under `/res/wallpapers` (e.g. `sunset-retro.png`),
and `SDE_WALLPAPER_MODE` to `center` (default), `tile`, `stretch` or `fill` to
use a wallpaper image. (The settings work, R4/G5, will drive this through the
WindowServer protocol instead of the environment.)

