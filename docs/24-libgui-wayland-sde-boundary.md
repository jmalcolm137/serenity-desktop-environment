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

### 3. The SDE config/state schema is baked into the framework

`libgui-wayland/src/WindowServerConnection.cpp` knows the SDE's config layout:

- `sde_state_dir()` — `SDE_STATE_DIR`, else `$XDG_CONFIG_HOME/sde`, else
  `~/.config/sde`;
- `windowserver_ini_path()` — reads `[Workspaces]` (grid), `[Input]` (ranges) from
  `WindowServer.ini`, and **writes `[Effects]` back**;
- `SDE_APP_BIN_DIR` in `LibWM.cpp` (`service_binary_path`) and
  `LaunchServerConnection.h` (`/bin/X` resolution).

An app framework should not know the desktop's config file or its environment
names. The compositor already serves much of this over the WindowServer protocol
(screen layout, effects, theme at greet), so the values can be served rather than
read from a file. The session app directory is a generic idea (a session-provided
app dir / `/bin` shadow) whose name and contract should not be SDE's.

Related: `patches/0019-libgui-desktop-wallpaper-state.patch` (the desktop
wallpaper state the compositor owns).

**Fix:** serve `[Workspaces]`/`[Input]`/`[Effects]` over the protocol (or isolate
the file schema behind an SDE-supplied seam), and generalise the app-dir
environment name.

### 4. SDE-specific patches in the port's patch set

- **Shell:** `0014-taskbar-system-menu`, `0039-taskbar-resolve-app-paths` and
  `0055-taskbar-window-grouping` are all Taskbar (shell) behaviour.
- **The SDE's own library:** `0015-terminal-shell-environment` and
  `0040-profiler-libserenitysystem` make the Lagom `Terminal` and `Profiler`
  targets depend on `${SDE_ROOT}/src/LibSerenitySystem` — the port's build knows
  about, and links, an SDE library. (`0014` also carries a "Serenity Desktop
  Environment host compat" tweak.)

**Fix:** move these into the SDE patch set, as was done for the Taskbar build.

### 5. Tests whose drivers live in the SDE

`libgui-wayland/src/test/{Notification,Tooltip,FontParity,BinPath,ComboBox,ProcStat}Test.cpp`
are payloads in the port, but every driver is an SDE script
(`scripts/run-*-test.sh` in this repo), so the port's suite cannot be exercised
without the SDE. Decide ownership: the port's own tests should be self-contained
in `libgui-wayland`; SDE-behaviour tests belong here.

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
- [ ] **3. Stop reading the SDE config from the framework** — serve
      `[Workspaces]`/`[Input]`/`[Effects]` over the WindowServer protocol (or
      isolate the schema), and generalise `SDE_APP_BIN_DIR`.
- [ ] **4. Move the SDE-specific patches** — the Taskbar ones (`0014`, `0039`,
      `0055`) and the `LibSerenitySystem` links (`0015`, `0040`) — into the SDE
      patch set.
- [ ] **5. Decide test ownership** (port self-contained vs SDE-driven).
