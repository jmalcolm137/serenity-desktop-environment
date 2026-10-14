# The libgui-wayland / SDE boundary: outstanding cleanup

**Status:** in progress · **Updated:** 2026-10-14
**Related:** [`06-service-layering.md`](06-service-layering.md) ·
[`03-engineering-conventions.md`](03-engineering-conventions.md) ·
`libgui-wayland/DESIGN.md`

`libgui-wayland` is an **application framework**: it runs *unmodified* SerenityOS
`LibGfx`/`LibGUI` applications on Wayland by replacing the `WindowServer`
(`LibWM`, the in-process WindowServer-protocol shim), plus the host seams the
Serenity userland needs. It is meant to be independent of any particular
compositor or desktop environment — its own `DESIGN.md` scopes it to "build
unmodified SerenityOS LibGUI/LibGfx applications … and run them natively on a
Wayland compositor".

Anything in it that only exists because of **this** desktop — the shell, the
session's config schema, the session's app directory — is an SDE concern that has
drifted into the framework. This records what was found and the work to move it
back across the line.

The same rule the rest of the project follows ([`06-service-layering.md`](06-service-layering.md)):
the *client library* stays a compositor/desktop-agnostic port; *service roles* and
*desktop policy* live in the SDE.

## Findings

### 1. The Taskbar build lives in the port's patch — and is duplicated

`libgui-wayland/patches/0001-lagom-build-full-libgui.patch` builds the shell:

```cmake
# Serenity Desktop Environment: the Taskbar. It is built when SDE_ROOT is set ...
if (DEFINED SDE_ROOT AND NOT "${SDE_ROOT}" STREQUAL "")
    add_serenity_subdirectory(Userland/Services/Taskbar)
    ...
```

The Taskbar is the desktop shell, and the SDE **already** carries this patch
(`patches/0003-sde-taskbar-build.patch`) — a straight duplicate. Worse, the SDE
copy is now *stale*: it still links `serenity-audio` (a target removed when the
AudioServer moved to the SDE) and its hunk context no longer matches, so it is
silently skipped as "already applied (overlapping context)".

**Fix:** delete the block from the port's `0001`; repair and keep the SDE's
`0003`, and make the SDE scripts that build the Taskbar apply the SDE patches
first (they currently assume the port's patch defines the target).

### 2. `NotificationServer` is built twice

The port defines and builds it (`libgui-wayland/src/CMakeLists.txt`, from
Serenity's `Userland/Services/NotificationServer`), and the SDE builds the
identical thing as a session service (`NotificationServer/`, staged into
`SDE_APP_BIN_DIR`, spawned on the notify portal). Notifications are the desktop's;
the port's copy is redundant and is not part of any port build target list.

**Fix:** drop the port's target; keep the SDE's.

### 3. The SDE config/state schema is baked into the framework — **partly done**

`libgui-wayland/src/WindowServerConnection.cpp` knows the SDE's config layout. The
config **file stays** — it is the desktop's — but the code that reads and writes
it belongs to the desktop, not the framework: the compositor owns the file and
serves the values over the WindowServer protocol.

**Done:** the shim no longer reads `WindowServer.ini`. The compositor sends the
values it owns over `serenity-window`:

- `serenity_window_manager.host_config(...)` (sent on bind) carries the workspace
  grid and the input-setting bounds; the shim's greet and its
  `get_mouse_acceleration_min`/`…_max`, `get_scroll_step_size_min`,
  `get_double_click_speed_min`/`…_max` answers come from it.
  `LibWM: host config: workspaces 3x4, …` shows the flow.

The session app-directory variable is also generalised: the framework reads
`LIBWM_APP_BIN_DIR` (generic) and still honours `SDE_APP_BIN_DIR` as its older
name, in `LibWM.cpp` and `LaunchServerConnection.h`.

**Remaining** (the same shape, a second pass):

- **Reads** — `input.ini` (mouse/keyboard values), `cursor.ini` (cursor theme /
  highlight), `screenlayout.ini` (the outputs), the wallpaper
  (`wallpaper.raw`/`.mode`/`.color`). Extend `host_config` (or add a settings
  channel) and have the compositor send them; they must not be read here.
- **Writes** — `set_system_effects` (`[Effects]`), `set_output_scale`
  (`[Output] DefaultScale`), the input/cursor setters, `apply_workspace_settings`
  (`[Workspaces]`), `set_theme`. These currently write the desktop's files; they
  need a request that hands the value to the compositor, which applies and
  persists it in its own config.
- `patches/0019-libgui-desktop-wallpaper-state.patch` (the wallpaper state the
  compositor owns) goes with the wallpaper read above.

The design: one settings channel — the compositor sends the values the shim must
answer with, and accepts the values the application sets. The keys are the
desktop's business; the shim only passes semantic settings across.

### 4. SDE-specific patches in the port's patch set — **done**

Five patches that only existed for this desktop used to live in
`libgui-wayland/patches`: the Taskbar ones (`0014-taskbar-system-menu`,
`0039-taskbar-resolve-app-paths`, `0055-taskbar-window-grouping`) and two that
make the Lagom `Terminal` and `Profiler` targets link the SDE's
`src/LibSerenitySystem` (`0015-terminal-shell-environment`,
`0040-profiler-libserenitysystem`).

They now live in this repo's `patches/` as `0001`–`0005`, applied before the
SDE's own patches (they were generated against the state before them), and are
verified to apply in order after the port's patch stack.

### 5. Tests whose drivers live in the SDE — **done**

The `libgui-wayland/src/test/*` payloads whose *only* driver is an SDE script
(`libwm-{binpath,combo,drag,font-parity,launch-exec,notify,procstat,services,tooltip}-test`)
now live here, in `test/`, and build via `scripts/build-tests.sh`. That is the
rule: a test belongs with the repo that owns its expectation. The port keeps the
payloads it drives itself (`libwm-{test-window,clipboard,about,fullscreen,submenu}-test`,
plus `appfile`/`hidpi-detail`).

## Not SDE-specific (leave in the port)

- `libeglgpu/` — a Mesa/EGL-backed `GPU::Device`.
- `compat/` — host stand-ins for Serenity-only headers.
- `Core::PortalServer` plumbing and spawning `WebContent`/`RequestServer`/`SQLServer`
  — the Browser application needs them.
- The port's own tests (`run-3dfileviewer/-clipboard/-menu/-pdfviewer/-headless-tests/-lib-tests`).

The **WindowManagerServer** (the Taskbar's protocol) is already on the SDE side —
the compositor serves it, not LibWM — so that separation is done.

## Plan

- [x] **1. Drop the port's `NotificationServer` target** (dead duplicate; the SDE
      builds the session service). `libgui-wayland/src/CMakeLists.txt`.
- [x] **2. Move the Taskbar build out of the port** into the SDE's `patches/0003`
      (repairing its stale context and dangling `serenity-audio` link). The port
      no longer adds the Taskbar target; the SDE builds it via
      `scripts/build-taskbar.sh` (which applies the SDE patches first), the
      scripts that need it use that, and `sde-session.sh` builds it if missing.
- [ ] **3. Stop reading the SDE config from the framework.** *Partly done:* the
      compositor now **serves the workspace grid and the input-setting bounds**
      over `serenity-window` (`serenity_window_manager.host_config`), and the
      session app-directory variable is generalised (`LIBWM_APP_BIN_DIR`, with
      `SDE_APP_BIN_DIR` still honoured). The input/cursor/screenlayout/wallpaper
      reads and the config **writes** remain — see §3.
- [x] **4. Move the SDE-specific patches** into this repo's patch set — the
      Taskbar ones and the `LibSerenitySystem` links (see §4).
- [x] **5. Decide test ownership** — the SDE owns the SDE-driven payloads;
      they moved to `test/` (`scripts/build-tests.sh`). See §5.
