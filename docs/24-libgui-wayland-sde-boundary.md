# The libgui-wayland / SDE boundary: outstanding cleanup

**Status:** done · **Updated:** 2026-11-19
**Related:** [`06-service-layering.md`](06-service-layering.md) ·
[`03-engineering-conventions.md`](03-engineering-conventions.md) ·
`libgui-wayland/DESIGN.md`

`libgui-wayland` is an **application framework**: it runs *unmodified* SerenityOS
`LibGfx`/`LibGUI` applications on Wayland by replacing the `WindowServer`
(`LibWM`, the in-process WindowServer-protocol shim), plus the host seams the
Serenity userland needs. It is meant to be independent of any particular
compositor or desktop environment: its own `DESIGN.md` scopes it to "build
unmodified SerenityOS LibGUI/LibGfx applications … and run them natively on a
Wayland compositor".

Anything in it that only exists because of **this** desktop (the shell, the
session's config schema, the session's app directory) is an SDE concern that has
drifted into the framework. This records what was found and the work to move it
back across the line.

The same rule the rest of the project follows ([`06-service-layering.md`](06-service-layering.md)):
the *client library* stays a compositor/desktop-agnostic port; *service roles* and
*desktop policy* live in the SDE.

## Findings

### Applications and their assets live in the desktop

`libgui-wayland` ships no application sources, resources or application-specific
patches; the applications are the desktop's. It keeps only the framework and the
test programs in `src/test/`. This repository defines the Lagom targets for the
applications it ships (`patches/0008-sde-lagom-targets.patch` now includes
`Calculator`, `PDFViewer` and `Piano`, which used to be built by libgui-wayland's
`apps/`), builds them (`scripts/build-apps.sh`, and `scripts/build-full-sde.sh` includes
the suite by default), and tests them against the real compositor
(`scripts/run-libgui-apps-test.sh`: build the compositor first, then the
applications, then run each headless). The Lagom targets are
`EXCLUDE_FROM_ALL`, so building the framework itself (or running `ninja` in the
shared Lagom tree) does not build any application; only the desktop's named
build does.

### 1. The Taskbar build lives in the port's patch, and is duplicated

`libgui-wayland/patches/0001-lagom-build-full-libgui.patch` builds the shell:

```cmake
# Serenity Desktop Environment: the Taskbar. It is built when SDE_ROOT is set ...
if (DEFINED SDE_ROOT AND NOT "${SDE_ROOT}" STREQUAL "")
    add_serenity_subdirectory(Userland/Services/Taskbar)
    ...
```

The Taskbar is the desktop shell, and the SDE **already** carries this patch
(`patches/0003-sde-taskbar-build.patch`): a straight duplicate. Worse, the SDE
copy is now *stale*: it still links `serenity-audio` (a target removed when the
AudioServer moved to the SDE) and its hunk context no longer matches, so it is
silently skipped as "already applied (overlapping context)".

**Fix:** delete the block from the port's `0001`; repair and keep the SDE's
`0003`, and make the SDE scripts that build the Taskbar apply the SDE patches
first (they currently assume the port's patch defines the target).

### 2. `NotificationServer` is built twice

The port defines and builds it (`libgui-wayland/src/CMakeLists.txt`, from
Serenity's `Userland/Services/NotificationServer`), and the SDE builds the
identical thing as a session service (`Services/NotificationServer/`, staged into
`LIBWM_APP_BIN_DIR`, spawned on the notify portal). Notifications are the desktop's;
the port's copy is redundant and is not part of any port build target list.

**Fix:** drop the port's target; keep the SDE's.

### 3. The SDE config/state schema is baked into the framework: **done**

`libgui-wayland/src/WindowServerConnection.cpp` knew the SDE's config layout. The
config **files stay** (they are the desktop's), but the code that reads and
writes them belongs to the desktop: the compositor owns them and serves the
values over the WindowServer protocol. The shim no longer opens any of them
(`WindowServer.ini`, `input.ini`, `cursor.ini`, `screenlayout.ini`,
`wallpaper.*`).

- `serenity_window_manager.host_config(...)` (on bind): the workspace grid and
  the input-setting bounds.
- `serenity_window_manager.host_settings(...)` (on bind, and re-sent after a
  change): the input values, the keyboard layout, the cursor theme/highlight,
  the theme and fonts, the effect choices, the workspace grid, the output
  layout, the wallpaper mode and the wallpaper file, as `key=value` lines (§7).
- `serenity_window_manager.set_host_setting(key, value)` and
  `set_wallpaper(...)`: an application's change; the compositor applies and
  persists it in its own config/state (`input.ini`, `cursor.ini`,
  `WindowServer.ini`, `screenlayout.ini`, `wallpaper.*`).

`scripts/run-setting-test.sh` covers the round trip; `run-output-test`,
`run-portal-test`, `run-effects-test`, `run-workspaces-test`,
`run-input-settings-test`, `run-cursor-test` and `run-settings-test` cover the
rest.

The session app-directory variable is generalised too: `LIBWM_APP_BIN_DIR`
(see §7 for the others).

### 4. SDE-specific patches in the port's patch set: **done**

Five patches that only existed for this desktop used to live in
`libgui-wayland/patches`: the Taskbar ones (`0014-taskbar-system-menu`,
`0039-taskbar-resolve-app-paths`, `0055-taskbar-window-grouping`) and two that
make the Lagom `Terminal` and `Profiler` targets link the SDE's
`src/LibSerenitySystem` (`0015-terminal-shell-environment`,
`0040-profiler-libserenitysystem`).

They now live in this repo's `patches/` as `0001`–`0005`, applied before the
SDE's own patches (they were generated against the state before them), and are
verified to apply in order after the port's patch stack.

### 5. Tests whose drivers live in the SDE: **done**

The `libgui-wayland/src/test/*` payloads whose *only* driver is an SDE script
(`libwm-{binpath,combo,drag,font-parity,launch-exec,notify,procstat,services,tooltip}-test`)
now live here, in `Tests/`, and build via `scripts/build-tests.sh`. That is the
rule: a test belongs with the repo that owns its expectation. The port keeps the
payloads it drives itself (`libwm-{test-window,clipboard,about,fullscreen,submenu}-test`,
plus `appfile`/`hidpi-detail`).

### 6. The applications are built by the SDE, not the framework: **done**

The SerenityOS applications (Applications, Demos, Games, DevTools) are the
desktop's, not the application framework's.

- **Build:** `scripts/build-apps.sh` builds them; `libgui-wayland`'s
  `scripts/build-libgui.sh` builds only the framework: `LibGfx LibGUI
  windowserver eglgpu`.
- **Target definitions:** they live in this repo's
  `patches/0008-sde-lagom-targets.patch` (the app list/loop, the games, the demos,
  the DevTools, Magnifier, and the Taskbar build), applied after the port's patch
  stack. `libgui-wayland`'s `0001`, the former `0035` (since removed as an empty stub; split: its `CodeComprehension` library
  hunk stays with the framework) and `0044` (CertificateSettings, an app) were
  trimmed or dropped accordingly.

### 7. A second pass (2026-11): **done**

The constraints for this pass: modify the SerenityOS source no more than before
(patches move between the repositories but do not grow), and add no round trips
or copies to the paths that matter for speed and memory.

- **The port read the SDE's files again.** The on-demand wallpaper read
  `$SDE_STATE_DIR/wallpaper.raw`, duplicating the compositor's state-directory
  logic and its file format, and every client read, wrote and inotify-watched the
  SDE's theme and font brokers (`sde-theme`, `sde-fonts`). Now the host settings
  carry `theme.path`/`theme.name` (the theme *in effect*, so high contrast still
  reaches clients), `fonts` and `wallpaper.file` (the path of the raw wallpaper,
  still read on demand and now straight into the bitmap, without a file-sized
  buffer). The brokers remain the SDE's own session interface: the compositor
  publishes a theme or font choice to them, their watchers apply, persist and
  broadcast, and the settings portal reads `sde-theme`. Each client lost two
  inotify watches, and a client that sets a wallpaper no longer keeps a private
  copy of it.
- **The protocol belongs to the port.** `serenity-window.xml` is libgui-wayland's,
  documents every host-settings key, and is copied here verbatim. Version 4 drops
  the `wallpaper` event, which no compositor sent any more.
- **SDE branding and application ports in the port's series.** The About-dialog
  branding and the port's 19 application patches (the Settings apps,
  FileManager, SystemMonitor, Debugger, CrashReporter, Terminal, ...) moved here
  as `0010`–`0029` (see [`../patches/README.md`](../patches/README.md)); their
  library halves (LibCore's process statistics, LibCoredump, LibELF) stayed in
  the port. The resulting SerenityOS tree is identical.
- **SDE names for generic mechanisms.** The variables both sides use are now
  `LIBWM_SESSION_DIR`, `LIBWM_WM_PORTAL_PATH`, `LIBWM_SESSION_BIN_DIR`,
  `LIBWM_APP_BIN_DIR` and `LIBWM_CLIENT_SCALE`, with no fallback to the old names:
  the two repositories are deployed together.

### Touch and window activation

LibWM maps `wl_touch` to the primary pointer (LibGUI has no touch event type),
so a tap or drag reaches widgets exactly like the mouse. For window activation
it binds `xdg-activation-v1`: a window opened from a click requests a token with
the click's serial, and a launcher may pass `XDG_ACTIVATION_TOKEN`. This
compositor already delivers `wl_touch` and focuses on `request_activate`, so a
window opened by a user action is raised. `scripts/run-libgui-touch-activation-test.sh`
runs a LibWM test window against the compositor for both paths.

### Screen capture

`serenity_window_manager.request_capture` / `captured` is the direct path: LibWM asks
for a region, this compositor answers from the next committed output frame, and the
shim caches the bitmap for the Magnifier, `shot` and the colour picker. A capture of a
static screen has no frame of its own, so `request_capture` schedules one; without
that, `shot` would wait for unrelated damage and time out. The colour picker samples a
one-pixel region through the same request.

On a compositor without `serenity-window`, LibWM falls back to the standard
`org.freedesktop.portal.Screenshot` (non-interactive, briefly cached; the
output-pixel PNG is cropped to the requested logical region); that path is the framework's
`libwm.settings-portal` suite. Here the direct path is covered by
`scripts/run-magnifier-test.sh` (the app, repainting) and
`scripts/run-libgui-capture-test.sh` (a static window, regions and the colour under
the pointer).

### Requests that have no client protocol

A few WindowServer requests describe things only the compositor can do. They are
not gaps in the shim: `show_screen_numbers` is forwarded as the
`effects.show_screen_numbers` host setting (an output overlay is the
compositor's to draw), `set_global_cursor_position` (the Taskbar putting the
cursor back on itself) uses `wp_pointer_warp_v1`, and global mouse tracking
(`GUI::MouseTracker`, the Eyes/CatDog demos) uses `ext-input-observation-v1`.
Both protocols are implemented by this compositor; the latter is a draft
(wayland-protocols MR !335) whose XML is vendored on both sides. The two copies
(`src/protocol/ext-input-observation-v1.xml` in libgui-wayland and
`Services/WindowServer/protocol/` here) must stay identical: change them
together. The compositor-side gate is that the client has bound
`serenity_window_manager`, so only session clients can observe the pointer. Resize
increments/aspect ratios and shadows around a surface have no Wayland client
protocol at all; window dragging and resizing are compositor-driven
(`xdg_toplevel.move`/`resize`) rather than tracked by polling the pointer.
Window stealing does not translate either: a client owns its surfaces and the
compositor owns stacking, so `set_window_parent_from_client`
(`xdg_toplevel.set_parent`) is the supported way to parent a transient.

### Desktop appearance and theme precedence

A theme sent over `serenity-window` (`theme.path`/`theme.name`) wins. Only when
no desktop sends one does LibWM follow the portable
`org.freedesktop.portal.Settings` appearance preference
(`org.freedesktop.appearance color-scheme`), selecting the bundled Dark, Light
or Default theme and following live changes. The SerenityDE session therefore
keeps full control of its theme; the portal path exists for sessions that do not
send one. Fonts, pointer/keyboard settings and the display layout have no
portable standard and are only available over `serenity-window`.

### Primary selection

SerenityOS has one clipboard; native toolkits also have PRIMARY (middle-click).
LibWM publishes a Serenity copy as both CLIPBOARD and PRIMARY and falls back to
PRIMARY when nothing owns CLIPBOARD, so text crosses both ways between Serenity
and native applications.

The wlroots implementation this compositor uses governs the semantics:
`wlr_primary_selection_v1` sends the selection only to the **keyboard-focused**
client (`seat->keyboard_state.focused_client`), and
`wlr_seat_request_set_primary_selection` validates the request's serial against
a serial the client received from a recent input event. A focused application
therefore sees the selection when it pastes, and can publish one after a
keypress or click; a window-less or unfocused client can do neither. Headless
tests must give the application a keyboard for that reason — this repository's
`scripts/run-primary-selection-test.sh` does, and checks the Serenity-to-native
direction here, while the framework's clipboard suite checks both directions
against its own test compositor (which does not filter by focus).

## Not SDE-specific (leave in the port)

- `libeglgpu/`: a Mesa/EGL-backed `GPU::Device`.
- `Libraries/Compat/`: host stand-ins for Serenity-only headers.
- `Core::PortalServer` plumbing and spawning `WebContent`/`RequestServer`/`SQLServer`:
  the Browser application needs them.
- The framework's own tests (`run-3d/-clipboard/-menu/-pdf/-headless-tests/-lib-tests`); the application tests moved here with the applications (`run-libgui-apps-test.sh`).

The **WindowManagerServer** (the Taskbar's protocol) is already on the SDE side:
the compositor serves it, not LibWM, so that separation is done.

## Plan

- [x] **1. Drop the port's `NotificationServer` target** (dead duplicate; the SDE
      builds the session service). `libgui-wayland/src/CMakeLists.txt`.
- [x] **2. Move the Taskbar build out of the port** into the SDE's `patches/0003`
      (repairing its stale context and dangling `serenity-audio` link). The port
      no longer adds the Taskbar target; the SDE builds it via
      `scripts/build-taskbar.sh` (which applies the SDE patches first), the
      scripts that need it use that, and `sde-session.sh` builds it if missing.
- [x] **3. Stop reading the SDE config from the framework.** The compositor serves
      everything the shim needs over `serenity-window` (`host_config`,
      `host_settings`, `wallpaper`) and accepts changes (`set_host_setting`,
      `set_wallpaper`), so the shim no longer opens any of the desktop's config or
      state files; the app-directory variable is generalised (`LIBWM_APP_BIN_DIR`).
      See §3.
- [x] **4. Move the SDE-specific patches** into this repo's patch set: the
      Taskbar ones and the `LibSerenitySystem` links (see §4).
- [x] **5. Decide test ownership**: the SDE owns the SDE-driven payloads;
      they moved to `Tests/` (`scripts/build-tests.sh`). See §5.
- [x] **6. Build the applications here, not in the framework**:
      `scripts/build-apps.sh`, with the target definitions in
      `patches/0008-sde-lagom-targets.patch`. See §6.
- [x] **7. Second pass**: settings over the protocol only, the protocol owned by
      the port, the application patches and branding here, generic variable
      names. See §7.
