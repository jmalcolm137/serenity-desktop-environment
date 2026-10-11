# Serenity Desktop Environment — R4 Implementation Plan

**Status:** Draft v0.3 · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) §6 (P8), §11 · [`04-known-gaps.md`](04-known-gaps.md) · [`07-systemserver-roles.md`](07-systemserver-roles.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md) §1, §4 · [`09-r3-implementation-plan.md`](09-r3-implementation-plan.md) §5

R0–R3 are done (see [`09-r3-implementation-plan.md`](09-r3-implementation-plan.md)).
This document is the execution plan for **R4 — Session & settings**. It owns both
scope and sequencing for R4 (unlike R3's `05`/`09` split), and it breaks R4 into
**additional milestones** so the work is ordered and testable. It is written
against the project-wide interoperability principle **P8** ([`01-objectives.md`](01-objectives.md) §6).

Each milestone lands with a **headless test** where the behaviour is scriptable
and **manual verification** where it is not (VT switch, real DRM, PAM). The
"prefer the session service, else in-process fallback" rule
([`06-service-layering.md`](06-service-layering.md)) still applies.

---

## 1. What R4 is, and the problem with the current shape

[`01-objectives.md`](01-objectives.md) §11 defines R4 as *"Login/autostart/lock/
power; display/input/appearance settings; Terminal + SystemMonitor."* That is a
large, mixed body of work with no ordering, and [`08-compositor-gaps.md`](08-compositor-gaps.md)
§1 notes R4 "owns no mechanism" for lock/power. The settings **UI already exists**
— SerenityOS `DisplaySettings` runs with all tabs (Background, Themes, Fonts,
Monitor, Workspaces, Effects) and only Background/Themes work today — so most of
R4 is building the **backends** the tabs call, not new UI.

Two things are pulled forward because they protect everything else
([`09-r3-implementation-plan.md`](09-r3-implementation-plan.md) §5):

- **Idle / inhibit / lock / DPMS** — a desktop you cannot lock is not a daily
  driver.
- **A supervisor / restart path** — a compositor fault currently takes the whole
  session with it (N4).

## 2. Cross-cutting principle: Wayland-first, standards-first

**Every R4 behaviour uses the standard Wayland protocol where one exists.** This
is P8 applied to R4, and it is a hard design rule for the milestones below, not a
preference. Concretely:

1. **Standard before `wlr-*` before private.** Use stable/staging `xdg_*`, `ext_*`
   and `wp_*` protocols; fall back to a `wlr-*` (wlroots) protocol or an
   SDE-private one **only when no standard exists**, and say why in the code/docs.
2. **Expose the standard globals.** Third-party clients must work unmodified —
   panels (`waybar`), lockers (`swaylock`), idle daemons (`swayidle`), clipboard
   managers (`wl-clipboard`), portals, screenshot tools. We already do this for
   session lock (R4.1); we must for workspaces, foreign toplevels and portals too.
3. **Our components are portable clients/users.** A component SDE ships (lock
   client, idle daemon, settings backend) must be usable **under other
   compositors** and must not depend on `serenity-window` or any SDE-only
   protocol. Symmetrically, SDE must accept third-party implementations.
4. **Keep the private surface minimal.** `serenity-window` stays scoped to
   Serenity shell parity (compositor-driven menubar, window placement,
   menu-dismiss); add a *standard* mechanism alongside whenever one exists.
5. **Track protocol status and migrate.** Where we currently use a `wlr-*`
   protocol that has a standard/staging successor (`wlr-data-control` →
   `ext-data-control-v1`, `wlr-screencopy` → `ext-image-copy-capture-v1`,
   `wlr-foreign-toplevel-management` → `ext-foreign-toplevel-list-v1` +
   `xdg-activation-v1`), adopt the successor as the primary and keep the old only
   for compatibility.

### 2.1 Note: "portal" means two different things

This project has two unrelated things called *portal*:

- **SerenityOS portals** — the in-process IPC endpoints under
  `/tmp/session/<id>/portal/…` and `/tmp/portal/wm` (`LibWM::make_portal()`,
  [`06-service-layering.md`](06-service-layering.md)). These are SDE's own.
- **`xdg-desktop-portal`** — the freedesktop D-Bus portal service that
  sandboxed/toolkit apps use (FileChooser, Screenshot, ScreenCast, RemoteDesktop,
  InputCapture, GlobalShortcuts, **Settings**, …).

**SDE does not use `xdg-desktop-portal` today.** Given P8 it should: the
**Settings** interface is needed by R4 (appearance), and the capture/input
interfaces are needed by R5. A wlroots compositor is served by
`xdg-desktop-portal-wlr` (screencast/screenshot) plus `xdg-desktop-portal-gtk`
(fallback), selected by `XDG_CURRENT_DESKTOP` and `portals.conf(5)`.

### 2.2 Protocol posture (as of wayland-protocols 1.49, 2026)

The pinned **wlroots 0.20.2** already ships managers for the newer standard
protocols, so adopting them is mostly wiring, not new protocol code. Status
legend: **stable** ⊂ wayland-protocols `stable/`; **staging** = ready for adoption;
**wlroots** = `wlr-*` (no standard yet); **spec** = a non-Wayland spec (D-Bus/XDG).

| Capability | Protocol (R4 choice) | Status | Milestone | Notes |
|---|---|---|---|---|
| Idle detection | `ext-idle-notify-v1` | staging | R4.1 | `wlr_idle_notifier_v1`; newer `get_input_idle_notification` ignores inhibitors |
| Idle inhibit | `zwp_idle_inhibit_v1` | unstable (standard) | R4.1 | no stable successor yet; widely used |
| Session lock | `ext-session-lock-v1` | staging | R4.1 | swaylock/gtklock/hyprlock conform — **done** |
| Output power / DPMS | `zwlr_output_power_management_v1` | wlroots | R4.1 | no standard yet; `wlopm` speaks it |
| Session save/restore | `xdg-session-management-v1` | staging | R4.2 | KDE/Firefox implementing; complements the supervisor |
| Autostart | XDG Autostart spec | spec | R4.2 | `*.desktop` in `autostart/` |
| Power, logout, reboot | `org.freedesktop.login1` (D-Bus), **optional** | spec | R4.2/R4.3 | logind opportunistic, never required; provider chain `SDE_POWER_CMD` → logind → fallbacks ([`12`](12-r4.3-portable-session-integration.md)) |
| Session/seat, VT switch | `libseat` (`logind`/`seatd`/`builtin`) | spec | R4.3 | `wlr_backend_autocreate` uses libseat; logind optional, `seatd` recommended ([`12`](12-r4.3-portable-session-integration.md)) |
| Transient seats | `ext-transient-seat-v1` | staging | R4.3/R4.6 | remote desktop, on-screen keyboard |
| **Appearance settings** | `org.freedesktop.portal.Settings` (+ backend) | spec (portal) | R4.4/R4.7 | `color-scheme`, `accent-color`, `contrast` → toolkits match the theme |
| Output management | `zwlr_output_management_v1` | wlroots | R4.5 | no standard equivalent yet |
| Fractional scaling | `wp-fractional-scale-v1` + `wp_viewporter` | staging + **stable** | R4.5/R6 | |
| Color management (HDR) | `wp-color-management-v1` | staging (v3) | R6 | GNOME 48+/KDE; out of R4 scope |
| Cursor shape | `wp-cursor-shape-v1` | staging (v2) | R4.7/R5 | theme/size still `XCURSOR_*` + compositor config |
| Workspaces | `ext-workspace-v1` | staging | R4.8 | `wlr_ext_workspace_v1`; `waybar` `ext/workspaces` speaks it |
| Foreign toplevels | `ext-foreign-toplevel-list-v1` | staging | R4.8/R5 | replaces `wlr-foreign-toplevel-management` |
| Activation / attention | `xdg-activation-v1` | staging | R4.2/R5 | launch/focus tokens; pairs with the above |
| Screen capture | `ext-image-copy-capture-v1` + `ext-image-capture-source-v1` | staging | R5 | replaces `wlr-screencopy` |
| Data control (clipboard mgr) | `ext-data-control-v1` | staging | R5 | replaces `wlr-data-control` |
| Shortcut inhibit | `zwp_keyboard_shortcuts_inhibit_manager_v1` | unstable | R5 | games |
| IME | `zwp_input_method_v2` + `wp_text_input_v3` | unstable | R5 | |
| **Portals (capture/input/shortcuts)** | `xdg-desktop-portal` (+ `-wlr`, `-gtk`) | spec (D-Bus) | R5 | Screenshot/ScreenCast/RemoteDesktop/InputCapture/GlobalShortcuts |
| Layer shell | `zwlr_layer_shell_v1` | wlroots | shell | no standard; universally supported |
| Decorations | `zxdg_decoration_manager_v1` | unstable | chrome | server-side decorations |
| Serenity shell parity | `serenity-window` (private) | SDE | R3/R4 | menubar, placement, menu-dismiss — keep minimal (P8.4) |

### 2.3 Serenity fidelity first (P9)

The parity target (§11) is a **floor, not a ceiling**. Where SerenityOS does
something better than other desktops — a richer service model (the `LaunchServer`
handler database, `NotificationServer`, `WindowManagerServer` semantics), a
distinct behaviour, or a component others lack — we **reproduce Serenity's
behaviour** rather than adopting another ecosystem's weaker equivalent. This is
**P9** ([`01-objectives.md`](01-objectives.md) §6).

When fidelity requires an app or service of our own, it is built as a
**standards-based Wayland/D-Bus citizen** (P8) that runs under **other
compositors too**, and we use **standard protocols, never another ecosystem's
private ones**. Fidelity is the goal; interoperable building blocks are how we
reach it. R4.9 carries the explicit addendum; the rule applies to every app and
service SDE ships (lock client, idle daemon, portal backend, launch/notify
services).

## 3. Additional milestones

R4 is delivered as **R4.1–R4.9**. Order is dependency-driven (see §4).

| # | Milestone | Outcome | Depends on |
|---|---|---|---|
| **R4.1** | **Session safety net** | Idle detection, idle inhibit, session lock and DPMS/dim *(compositor side landed)* | — |
| **R4.2** | **Session lifecycle & supervisor** | Session start/autostart, logout/reboot/shutdown, compositor restart + shell relaunch, save/restore *(supervisor/autostart/power + `xdg-session-management-v1` restore landed)* | R4.1 |
| **R4.3** | **Portable session integration** | logind/systemd **optional** (detected, never required): standard env, seat/VT via `libseat` (`seatd`/`builtin`), power provider chain, optional D-Bus/portals — [`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md) | R4.2 |
| **R4.4** | **Compositor config & live reload** | `WindowServer.ini` schema + validated live reload; Settings-portal exposure | — |
| **R4.5** | **Display / output settings** (landed; fractional scale deferred) | Mode/scale/rotation/arrangement backend (Monitor tab) + fractional scale | R4.4 |
| **R4.6** | **Input settings & keymaps** (landed) | libinput device config + keyboard layouts/keymap + MouseSettings | R4.4 |
| **R4.7** | **Appearance, effects & fonts** (landed) | Scrollable fonts/zoom, Effects, cursor; exposed via Settings portal | R4.4 |
| **R4.8** | **Workspaces & toplevels** (landed) | Workspaces tab drives the grid; expose `ext-workspace-v1` + `ext-foreign-toplevel-list-v1` | R4.4 |
| **R4.9** | **Applications enablement** | Terminal, SystemMonitor, CrashReporter/Debugger feasibility | R4.1–R4.3 |

### R4.1 — Session safety net

Compositor-side, all standard (wlroots 0.20 provides the pieces):
`ext-idle-notify-v1` (`wlr_idle_notifier_v1`), `zwp_idle_inhibit_v1`
(`wlr_idle_inhibit_manager_v1`), `ext-session-lock-v1`
(`wlr_session_lock_manager_v1`) and DPMS (`wlr_output_power_manager_v1`; plus
dim). `wlr_idle_notifier_v1_notify_activity()` runs on every input event and
`wlr_idle_notifier_v1_set_inhibited()` while an inhibitor is present.

The compositor implements the **`ext-session-lock-v1` contract**: one lock at a
time; a lock surface configured per output (including hotplug while locked);
`locked` once every output is covered; no input or normal surfaces shown while
locked; `unlock` tears the lock down. **SDE does not ship its own lock client**:
the compositor is a standard server, so any conformant locker works. The session
uses **swaylock** today (`SDE_LOCK_CMD`); a native SDE lock client is a documented
future deliverable (§10) and must itself be a plain `ext-session-lock-v1` client.

**Interop rule (P8).** Do **not** build idle-to-lock into the compositor. Expose
`ext-idle-notify-v1` + `ext-session-lock-v1` so the standard stack — `swayidle` +
`swaylock` — works, and optionally ship a portable SDE idle daemon (`R4.2`).

- **Exit test:** `scripts/run-idle-test.sh` (idle fires; an inhibitor suppresses
  it; DPMS off/on) and `scripts/run-lock-test.sh` (a standard lock client covers
  every output and gets `locked`; `Super+L` spawns the configured lock command).

**Landed (compositor side).** `WindowServer` creates the idle-notifier,
idle-inhibit, output-power and session-lock managers; notifies activity on key,
pointer, button and axis input; suppresses idle while inhibited; turns outputs
off/on; and implements the lock contract (per-output lock surfaces, `locked` when
all outputs are covered, input routed to the lock surface on the output under the
cursor). `Super+L` spawns the lock client (`SDE_LOCK_CMD`, default `swaylock`).
Both scripts pass, including a two-output lock.

**R4.1 complete.** Blank-before-lock and lock-surface reconfiguration landed:
`tools/sde-dpms.c` sets output power (blank/restore) and `sde-idle` gained a
`resume` command, so the session arms *blank at N s, lock at M s, restore on
activity* (`SDE_SESSION_IDLE_DPMS_SECONDS`); the compositor re-configures a lock
surface when its output's size changes while locked (guarded against the
per-frame `output.commit`). Verified by `scripts/run-idle-test.sh` (blank +
resume) and `scripts/run-lock-test.sh` phase (C) (resize while locked →
`surface configured 1024x768`).

### R4.2 — Session lifecycle & supervisor

- **Autostart:** a session init that launches the shell (Taskbar, Desktop) plus
  **XDG Autostart** entries (`$XDG_CONFIG_HOME/autostart/*.desktop`), replacing
  the hand-rolled startup command in `scripts/start-sde-session.sh`.
- **Power:** Log out / Reboot / Shut down from the Taskbar system menu
  (`ShutdownDialog` spawns `/bin/shutdown`, `/bin/reboot`, `/bin/logout` — route
  these to **`org.freedesktop.login1`** D-Bus calls, not host binaries).
- **Supervisor:** wrap the compositor; on exit/fault restart it and relaunch the
  shell. Pair with **`xdg-session-management-v1`** (staging) so clients that
  opt in restore their toplevels across a restart, rather than relying on SDE
  keeping a private window list.
- **Idle-to-lock:** ship a small, portable idle daemon (or document using
  `swayidle`) driven by R4.1's `ext-idle-notify-v1`.

- **Exit test:** `scripts/run-session-test.sh` — autostart entries launch; power
  actions are routed (mocked) and asserted; the supervisor restarts a killed
  compositor.

**Landed (partial).** `scripts/sde-session.sh` is the session supervisor: it sets
the session environment (`XDG_SESSION_TYPE`, `XDG_CURRENT_DESKTOP`,
`WAYLAND_DISPLAY`), runs **XDG Autostart** entries (`$XDG_CONFIG_HOME/autostart`,
honouring `Hidden=true` and field-code stripping), starts the compositor, and
restarts it on a non-zero exit within a bounded budget. It arms **idle-to-lock**
by running the portable **`sde-idle`** daemon (`tools/sde-idle.c`, built by
`scripts/build-tools.sh`) — a plain `ext-idle-notify-v1` client that runs the lock
command after `SDE_SESSION_IDLE_SECONDS`. `scripts/sde-power.sh` routes
`logout|reboot|shutdown|suspend` to **logind** (`loginctl`, else `busctl` on
`org.freedesktop.login1`), with `SDE_POWER_CMD` as the seam.
`scripts/start-sde-session.sh` now delegates to the supervisor.
`scripts/run-session-test.sh` and the extended `scripts/run-idle-test.sh` pass.

**Power routing landed.** The supervisor writes `sde-power` **`/bin` shadow**
helpers (`shutdown`, `reboot`, `logout`, `suspend`) into a session bin dir and
exports `SDE_SESSION_BIN_DIR`; the port's
`Core::Process::resolve_executable_path()` consults that dir **first** (port
change, `patches/0011`), so the unmodified Taskbar `ShutdownDialog`
(`/bin/shutdown --now`, `/bin/reboot`, `/bin/logout`) now routes to logind. Verified
by `scripts/run-binpath-test.sh` (port-side `/bin` shadow) and
`scripts/run-session-test.sh` phase (E).

**Bug fixed along the way (R4.1):** releasing an idle inhibitor left the seat
*permanently inhibited* — wlroots unlinks an inhibitor only *after* its destroy
signal, so the compositor's `wl_list_empty` check still saw it. The compositor now
excludes the dying inhibitor when recomputing the inhibited state
(`WindowServer/src/main.cpp`).

**Session restore landed (`xdg-session-management-v1`).** The compositor serves the
standard protocol (`WindowServer/src/SessionManager.cpp`): `get_session` (new →
`created`, or load → `restored`; a second client taking an in-use id gets
`replaced`), `add_toplevel`/`restore_toplevel`/`remove_toplevel`,
`xdg_toplevel_session_v1.restored`, and per-session geometry persisted under
`$SDE_STATE_DIR/sessions/<id>` (else `$XDG_RUNTIME_DIR/sde-sessions`). Toplevel
position and size are captured as windows move/resize and reapplied on restore
(size deferred to the initial commit, where `set_size` is safe). wlroots 0.20 has
no manager for this protocol and hides the `xdg_toplevel` interface, so the
compositor generates xdg-shell's server code for the interface symbol and
resolves the toplevel via `wlr_xdg_toplevel->resource`. Verified by
`scripts/run-session-management-test.sh`, which **restarts the compositor**
between creating and restoring a session.

### R4.3 — Portable session integration (logind/systemd optional)

**Reframed — see
[`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md).**
logind and systemd are **optional, runtime-detected accelerators — never
dependencies** (P4). The session must start, run, lock and (where the OS permits)
power off with **no logind/systemd at all**, degrading gracefully. In summary:

- **Environment:** always set `XDG_SESSION_TYPE`, `XDG_SESSION_CLASS`,
  `WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP`/`XDG_SESSION_DESKTOP`,
  `DESKTOP_SESSION`; push them onto the **D-Bus activation environment** only if a
  session bus and helper exist. **No `graphical-session.target`** — the supervisor
  (`scripts/sde-session.sh`) owns ordering.
- **Seat / DRM / VT:** keep using **`libseat`** (`logind`/`seatd`/`builtin`
  backends; `seatd` recommended, `builtin` for a bare TTY). Never reimplement DRM
  master or VT switching. Nested needs no seat at all.
- **Power:** an ordered **provider chain** in `sde-power.sh` — `SDE_POWER_CMD` →
  logind (D-Bus) → portable fallbacks → *unavailable* — behind the existing `/bin`
  shadow helpers, so the unmodified Taskbar menu stays provider-agnostic.
- **Portals / D-Bus:** **optional**. `xdg-desktop-portal` activates over D-Bus
  where present; absent portals must not break the session (the in-process
  Serenity shims carry on). `ext-transient-seat-v1` stays future/optional.

- **Exit tests:** headless with logind/systemd/D-Bus absent (mocked) — standard env
  set, provider chain selected, capability probe correct, graceful degradation —
  plus manual verification on real hardware for each `libseat` backend (`seatd`,
  `builtin`) and nested. Details in
  [`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md) §5.

**Landed.** `scripts/sde-session.sh` sets the standard session environment
(forcing SDE identity even when nested) and best-effort pushes it to the D-Bus
activation environment (`SDE_DBUS_ENV_CMD` seam; never required). `scripts/sde-power.sh`
is now an **ordered provider chain** (`SDE_POWER_CMD` → logind → portable
fallbacks → *unavailable*) with `--check`/`--list`; the logout fallback stops the
supervisor via its pid file. `scripts/sde-capabilities` probes seat
(`logind`/`seatd`/`builtin`)/logind/D-Bus/portal and reports `power.*`
availability (`key=value` or `--json`). Verified by
`scripts/run-session-env-test.sh`, `scripts/run-power-providers-test.sh`,
`scripts/run-capabilities-test.sh`, and a degraded phase in
`scripts/run-session-test.sh`. Manual real-hardware seatd/builtin verification
remains.

### R4.4 — Compositor configuration & live reload

Extend the R3 `WindowServer.ini` (`[Workspaces]`, `[Applet]`) into a documented
schema for **keybindings, focus policy, snap/tile behaviour, workspace grid,
effects, output defaults**, with validation and live reload, plus a
`scripts/sde-config` helper. This is the seam R4.5–R4.8 write through.

**Interop (P8).** Expose the appearance subset through an
**`org.freedesktop.portal.Settings` backend** (`color-scheme`, `accent-color`,
`contrast`) so GTK/Qt/Flatpak apps match the SDE theme — the standard channel
toolkits already read. Also keep the `serenity-window`/theme-broker path for the
Serenity shell.

- **Exit test:** changing a keybinding / grid / effect takes effect live; invalid
  config is rejected with a clear error; a Settings-portal query returns the SDE
  colour scheme.

### R4.5 — Display / output settings

Implement the WindowServer protocol `get_screen_layout` / `set_screen_layout`
(previously a single-output stub) against **`zwlr_output_management_v1`** (no
standard equivalent yet), so DisplaySettings ▸ **Monitor** works: mode, scale,
rotation, arrangement, primary; persisted. Pair integer scaling with
**`wp-fractional-scale-v1`** + `wp_viewporter` (stable) for fractional scale where
the host supports it.

- **Exit test:** `scripts/run-output-test.sh` — a scripted layout change
  reconfigures the (headless) output; manual on DRM.

> **Landed (R4.5).** The compositor persists the live layout to
> `screenlayout.ini` in the state directory (in the Serenity WindowServer
> ScreenLayout format) whenever outputs are created or a
> `zwlr_output_management_v1` configuration is applied, and it applies a saved
> entry's resolution/scale/position when an output is created. Edits to the file
> are watched and re-applied live. The port's
> `get_screen_layout`/`set_screen_layout` read/write that same file, so
> DisplaySettings ▸ **Monitor** now shows the real output (device name,
> resolution, display scale) and drives the compositor. Verified by
> `scripts/run-output-test.sh` and a Monitor screenshot against a headless
> session on `HEADLESS-1`.
>
> **Deferred:** `wp-fractional-scale-v1` + `wp_viewporter` fractional scale, and
> rotation/primary persistence — the Serenity ScreenLayout model has no rotation
> field, so those land with the LibWindowServer backing-store scale work (R6).

### R4.6 — Input settings & keymaps

`libinput` device configuration (accel profile/speed, tap-to-click, natural
scroll, button mapping, disable-while-typing, left-handed) with persistence, and
real keyboard management: `set_keymap`, multiple layouts, per-device layout.
Enable `MouseSettings`.

**Interop (P8).** Device config is compositor-side (no protocol); expose
**`wlr-virtual-keyboard`/`wlr-virtual-pointer`** for on-screen keyboards and the
test harness, and plan **`ext-transient-seat-v1`** for virtual seats. Client-facing
shortcut inhibition (`zwp_keyboard_shortcuts_inhibit_manager_v1`) and IME
(`zwp_input_method_v2`/`wp_text_input_v3`) are R5.

- **Exit test:** `scripts/run-input-settings-test.sh` asserts config is applied to
  a (virtual) device; manual for real hardware.

> **Landed (R4.6).** Input settings live in `input.ini` in the state directory
> (`[Mouse]` acceleration, scroll step, double-click speed, buttons-switched,
> natural-scroll, tap-to-click, left-handed, disable-while-typing; `[Keyboard]`
> layouts, variant, model, options, repeat delay/rate). The compositor applies
> them to libinput devices (`wlr_libinput_get_device_handle`, gated by
> `wlr_input_device_is_libinput`) and to the seat/event path (natural scroll,
> scroll step, buttons-switched, double-click speed); it builds the xkb keymap
> from the layouts and re-applies everything on change.
>
> MouseSettings is built as part of SDE (alongside DisplaySettings) and wired
> through the WindowServer protocol: `get/set_mouse_acceleration`,
> scroll step, double-click speed, mouse-buttons-switched and natural-scroll read
> and write `input.ini`, and new `get_*_min/max` methods carry the ranges from the
> config schema (`[Input]` in `WindowServer.ini`), so the UI does not hardcode
> them. The Cursor Theme/Highlight tabs are backed by `cursor.ini` (applied to the
> real cursor in R4.7). Verified by `scripts/run-input-settings-test.sh` and a
> MouseSettings screenshot.
>
> KeyboardSettings is adapted from the SerenityOS kernel keymap path to xkb: it
> reads the current layouts and the available xkb layout names from the port
> (`get_keyboard_layout`/`get_keyboard_layout_names`) and applies changes by
> writing `input.ini [Keyboard]` (`set_keyboard_layout`), instead of the
> modeless `/sys/kernel/keymap`, `/etc/Keyboard.ini` and `/bin/keymap`.

### R4.7 — Appearance, effects & fonts

Close the R3 font deferral ([`10-font-parity-decision.md`](10-font-parity-decision.md)):
a **session font broker** (like the theme broker) so `set_system_fonts` applies a
chosen family/size to every client + the compositor, with a scalable default so
**zoom works**; wire DisplaySettings ▸ **Fonts**. Also Effects (animations,
shadows, tooltip shadow, window geometry) via `SystemEffects` live update, and
cursor settings (theme/size via `XCURSOR_*`; client cursors via
**`wp-cursor-shape-v1`**).

**Interop (P8).** Publish the chosen appearance through the **Settings portal**
(R4.4) so non-Serenity apps follow; keep the theme broker for the Serenity shell.

- **Exit test:** `scripts/run-fonts-test.sh` — a system-font change applies and
  TextEditor zoom changes the size (update `libwm-font-parity-test`);
  `scripts/run-effects-test.sh` — an effect toggles live.

> **Landed (R4.7).** A session **font broker** (`$XDG_RUNTIME_DIR/sde-fonts`)
> applies a chosen default / fixed-width / window-title font to every client
> (`set_system_fonts`, and `fast_greet` on connect) and to the compositor, and
> persists it to `WindowServer.ini [Fonts]`. The default stays the bundled bitmap
> **Katica** (Csilla fixed-width), matching upstream SerenityOS; picking a
> scalable family (SerenitySans) is what makes size-based zoom resolve. Display
> Settings ▸ **Fonts** reflects the session font and applies through the broker.
>
> **Effects:** Display Settings ▸ Effects reads `WindowServer.ini [Effects]`;
> `set_system_effects` applies to the client and writes `[Effects]`
> (`ShowGeometry`/`TileWindow` plus the toggles), which the compositor applies
> live (the geometry HUD honours the mode). **Cursor:** the compositor applies the
> XCursor theme/size and the highlight (a translucent ellipse behind the cursor)
> from `cursor.ini`, publishes `XCURSOR_*`, and exposes `wp-cursor-shape-v1` for
> client cursor shapes; Mouse Settings' Cursor Theme / Cursor Highlight tabs write
> it.
>
> **Theme:** Display Settings ▸ Themes applies through the theme broker; the
> compositor persists the choice to `WindowServer.ini [Theme]` and applies it at
> startup (clients follow via the broker/`[Theme]`).
>
> Verified by `scripts/run-fonts-test.sh`, `scripts/run-effects-test.sh`, and
> Fonts/Effects tab screenshots. Also fixed a live-reload bug where the config
> watchers added a file watch before the file existed, so only the first change
> reloaded (affects R4.4–R4.6 watchers too).

### R4.8 — Workspaces & toplevels

Connect DisplaySettings ▸ **Workspaces** to the compositor grid (`[Workspaces]`),
applied live and persisted.

**Interop (P8) — the important one.** Also implement the standard
**`ext-workspace-v1`** (via `wlr_ext_workspace_v1`) and
**`ext-foreign-toplevel-list-v1`**, mapping them onto SDE's workspaces and window
list, so third-party panels (`waybar`) and automation work. Keep the Serenity
`WindowManagerServer` path for the unmodified Taskbar/WorkspacePicker, but the
standard protocols become the primary interop surface. Pair activation with
**`xdg-activation-v1`**.

- **Exit test:** apply a grid change; the workspace picker and switching match;
  `waybar` (or a scripted `ext-workspace-v1` client) sees SDE's workspaces.

> **Landed (R4.8).** Display Settings ▸ Workspaces reads the live grid and
> `apply_workspace_settings` writes `[Workspaces]`, which the compositor
> live-reloads and persists. The standard **`ext-workspace-v1`**
> (`wlr_ext_workspace_v1`) publishes the grid — one group for the outputs and one
> workspace per cell, with grid coordinates and the active state — and activation
> requests switch the compositor. **`ext-foreign-toplevel-list-v1`** publishes the
> window list (title/app_id), and **`xdg-activation-v1`** focuses a requesting
> toplevel and switches to its workspace. The Serenity `WindowManagerServer` path
> stays for the unmodified Taskbar/WorkspacePicker. Verified by
> `scripts/run-workspaces-test.sh` (a scripted `ext-workspace-v1` client) and a
> Workspaces-tab screenshot.

### R4.9 — Applications enablement

`Terminal` runs; make **SystemMonitor** work (`/proc` mapping), and assess
`CrashReporter` / `Debugger` feasibility (both may need shims). Deliver what is
reasonable; record what is not.

**Interop (P8).** Prefer `xdg-desktop-portal` interfaces over SDE-private
services where an app expects a portal (e.g. OpenURI/FileChooser via `-gtk`).

**Addendum (P9 — Serenity fidelity first).** We optimise for **fidelity to how
SerenityOS works**, especially where SerenityOS does *more* than other solutions
— e.g. a real `LaunchServer` with a shared handler database, `NotificationServer`,
`WindowManagerServer` semantics, the applet model, `SystemMonitor`. If matching
SerenityOS means writing an app or service ourselves, we do, and we build it as a
**standards-based Wayland/D-Bus component that other compositors can use too** —
**standard protocols, not another ecosystem's private protocols** (no
`org_kde_kwin_*`, no GNOME-private interfaces). Concretely:

- Serenity services we stand in for on Linux (launch, notify, applets, WM
  semantics) are implemented as portable services/daemons, not as SDE-only
  sockets; where a standard exists (portal, `ext-workspace-v1`, D-Bus), use it.
- Apps that SerenityOS ships but Linux lacks equivalents for are **ported**
  (unmodified where possible) rather than replaced by a Linux app with different
  behaviour — matching P2.
- Where another ecosystem's *private* protocol would be the easy path (e.g. a
  KDE/GNOME-specific service interface), prefer the standard protocol or our own
  interoperable one, even at some cost.

- **Exit test:** the app launches in the session and shows real data; any service
  it needed that we wrote is exercised by a standard client (not an SDE-private
  one) and, where feasible, briefly verified under a second compositor.

## 4. Sequencing & critical path

```
 R4.1 safety net ──▶ R4.2 lifecycle/supervisor ──▶ R4.3 portable session
      │                                                 │
      └──────────────────┬──────────────────────────────┘
                         ▼
 R4.4 compositor config ─┬─▶ R4.5 display/output
 (Settings-portal seam)  ├─▶ R4.6 input/keymaps
                         ├─▶ R4.7 appearance/effects/fonts
                         └─▶ R4.8 workspaces (+ ext-workspace-v1)
                                          R4.9 apps (independent; after R4.1–R4.3)
```

**Start with R4.1** (done, compositor side), then **R4.2/R4.3** (session +
portals), then **R4.4** (the config/portal seam) and the settings backends.
R4.9 can run alongside once the session is safe and startable.

## 5. De-risking spikes (time-boxed, before committing)

| Spike | Question | Decision it unblocks |
|---|---|---|
| **Lock client** | SerenityOS has no lock screen; port one, write one, or use an external locker? | R4.1 — **resolved: use a standard client (swaylock) now**; native client deferred (§10) |
| **Restart semantics** | What can the supervisor preserve across a restart? Use `xdg-session-management-v1` or a private restore? | R4.2 scope |
| **Portals** | Which `xdg-desktop-portal` backend(s) and `portals.conf` for SDE? Does `-wlr` suffice once capture protocols land? | R4.3/R5 |
| **Output management on nested vs DRM** | How much of the Monitor tab is meaningful headless/nested vs real hardware? | R4.5 exit test |
| **Font broker** | Broker file shape and relayout trigger for a live system-font change | R4.7 |
| **logind availability** | Is logind present in the target environments, and what is the fallback? | R4.2/R4.3 — **resolved:** logind is optional; portable providers (`seatd`/`builtin`, power chain) in [`12`](12-r4.3-portable-session-integration.md) |

## 6. Testing strategy

- **Compositor behaviours** (idle, inhibit, lock, DPMS, output, input, effects)
  get scripted clients under the headless backend, asserting compositor log lines
  and, where useful, state dumps — the pattern R3 established
  (`run-input-test.sh`, `run-applet-test.sh`, `run-tooltip-test.sh`).
- **Standard-protocol interop** gets its own generic clients (a lock client, an
  `ext-workspace-v1` client, an `org.freedesktop.portal.Settings` query) so we
  prove third-party compatibility, not just self-consistency.
- **Settings round-trips** (font broker, output layout, input config) assert the
  state is written, applied, and persists across a restart.
- **Session/power/VT/PAM/logind** are manual on real hardware.

## 7. Exit criteria (R4 definition of done)

R4 is done when:

1. the session can be **locked** and **idles** to lock/DPMS, with idle inhibition
   honoured, using standard `ext-idle-notify-v1`/`ext-session-lock-v1`;
2. the session **starts, autostarts, logs out and powers off/reboots** correctly,
   and a compositor fault is **recovered**;
3. the session is a proper Wayland session for portals/apps (standard env,
   D-Bus activation env pushed where present) **without requiring logind or
   systemd**, degrading gracefully when `xdg-desktop-portal` is absent
   ([`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md));
4. DisplaySettings is **functional end to end** — Monitor, Fonts (working zoom),
   Input, Appearance/Effects, Workspaces — driven by a documented,
   live-reloadable config, with appearance exposed via the Settings portal;
5. **standard protocols are exposed** so third-party tools work: `swaylock`,
   `ext-workspace-v1` panels (`waybar`), `wl-clipboard`, `xdg-desktop-portal`;
6. the **KDE Plasma (KWin) parity target** (§11) is met for the R4-scoped rows —
   `xdg-session-management-v1`, `viewporter`, `wp-fractional-scale-v1`,
   `wp-cursor-shape-v1`, `ext-workspace-v1`, `ext-foreign-toplevel-list-v1`,
   `xdg-activation-v1` — with the rest scheduled in R5/R6;
7. `Terminal` and (at least) `SystemMonitor` run;
8. every scriptable behaviour has a headless test, and the R1–R3 suites still
   pass.

## 8. Risks and decisions

| Risk / decision | Owner | Note |
|---|---|---|
| Standard-vs-`wlr-*` for output management | R4.5 | No standard yet; `zwlr_output_management_v1` is de-facto — revisit as a standard lands |
| A standard workspace protocol would supersede the private one | R4.8 | Adopt `ext-workspace-v1` as primary **and** keep `WindowManagerServer` for parity |
| No SerenityOS lock screen | R4.1 | Use a standard locker; native client is future work (§10) |
| Lock needs authentication (PAM) | R4.1 | Native client only; swaylock already does PAM |
| `xdg-desktop-portal` coverage | R4.3/R5 | `-wlr` for capture; `-gtk` fallback; Settings backend of our own for R4 |
| logind/systemd may be absent | R4.2/R4.3 | Now a **first-class supported mode**, detected and never required ([`12`](12-r4.3-portable-session-integration.md)) |
| `ext-session-lock-v1` on hotplug/scale | R4.1 | Must cover every output, including new ones |
| Font change requires live relayout | R4.7 | Cross-repo (`libgui-wayland`); broker like the theme |
| `CrashReporter`/`Debugger` may need shims | R4.9 | Feasibility-assess; drop if unreasonable |

## 9. Out of scope for R4

R5/R6 breadth — XWayland, capture/screencast, data-control, IME/accessibility,
HiDPI/fractional scaling **implementation**, multi-GPU, color management,
packaging, CI matrix, sanitizers/fuzzing — remains where
[`08-compositor-gaps.md`](08-compositor-gaps.md) assigns it. Do not pull R5/R6
work into R4 (the protocol *choices* above are noted so R4 does not paint R5 into
a corner).

## 10. Deferred / future deliverables

Documented so they are not lost; none blocks R4.

- **An SDE `xdg-desktop-portal` backend.** To reach **portal parity with KDE** we
  cannot lean on `xdg-desktop-portal-wlr` + `-gtk` alone; `xdg-desktop-portal-kde`
  implements a broad set (Settings, GlobalShortcuts, Inhibit, InputCapture,
  RemoteDesktop, ScreenCast, Clipboard, Wallpaper, Background, Notification,
  FileChooser, AppChooser, Print, Secret, Email, DynamicLauncher, Access,
  Account). Plan: ship a small **SDE portal backend** implementing at least
  **Settings** (R4) and then **GlobalShortcuts / RemoteDesktop / InputCapture /
  ScreenCast / Screenshot** (R5), composed with `-wlr`/`-gtk` for the rest via
  `portals.conf`. This can land **after** R4; it is the missing half of "portal
  parity" (§11).
- **A native SDE lock client.** SDE uses **swaylock** (or any conformant locker)
  today: the compositor is a standard `ext-session-lock-v1` server. A native
  client is future work and **must be a plain Wayland `ext-session-lock-v1`
  client** — no SDE-specific protocols — so it runs under **any** compositor
  (sway, Hyprland, KWin, SDE) and so SDE keeps working with third-party lockers.
  Intended shape: Serenity-themed screen over every output (per-output wallpapers,
  dim/blur); **PAM** authentication; multi-output + hotplug; idle-to-lock. Until
  then `SDE_LOCK_CMD` (default `swaylock`) selects the client.
- **A portable SDE idle daemon** (`swayidle`-equivalent) for idle-to-lock/blank.
- **Serial console / multi-seat, VT-switch polish, suspend/resume hooks** — R4.3.

## 11. Parity target: KDE Plasma (KWin)

**Goal.** SDE's **standard** Wayland protocol surface should match the latest
**KDE Plasma / KWin** (`v6.7.5`, the current stable at the time of writing; 6.8 in
beta), so that applications, toolkits, portals and tooling behave the same as on
Plasma. Reference: KWin's `src/wayland/` implementation set (v6.7.5 + master) and
`xdg-desktop-portal-kde`.

**Rules.** (a) Parity means the **standard/staging** protocols KWin exposes — the
KDE-private protocols (below) are **not** parity targets, because they only serve
KDE's own shell; SDE keeps its own minimal `serenity-window` for that role.
(b) Where KWin uses a private protocol for a job that has a standard one, we
prefer the **standard** (P8) — SDE may thus *exceed* KWin on interoperability.
(c) Parity is scoped by milestone: R4 covers the session/appearance subset it
needs; the rest is R5/R6.

Legend: **✓** have · **↗** planned R4 · **→** planned R5 · **·** planned R6 ·
**–** KWin uses a private protocol · **n/a** not applicable.

| Protocol | KWin | SDE | Milestone |
|---|---|---|---|
| `xdg-shell` | ✓ | ✓ | — |
| `xdg-decoration` | ✓ | ✓ | — |
| `xdg-output` | ✓ | ✓ | — |
| `wlr-layer-shell` | ✓ | ✓ | — |
| `linux-dmabuf` | ✓ | wlroots | R6 |
| `presentation-time` | ✓ | ✗ | R6 |
| `tablet-v2` | ✓ | ✗ | R5 |
| `viewporter` | ✓ | needed (client uses it) | R4.5 |
| `xdg-activation-v1` | ✓ | ✗ | R4.2 / R5 |
| `xdg-session-management-v1` | ✓ | ✗ | **R4.2** |
| `xdg-dialog-v1` | ✓ | ✗ | R5 |
| `xdg-foreign-v2` | ✓ | ✗ | R5 |
| `xdg-system-bell-v1` | ✓ | ✗ | R5 |
| `xdg-toplevel-drag-v1` | ✓ | ✗ | R5 |
| `xdg-toplevel-icon-v1` | ✓ | ✗ | R5 |
| `xdg-toplevel-tag-v1` | ✓ | ✗ | R5 |
| `xwayland-shell-v1`, `xwayland-keyboard-grab-v1` | ✓ | ✗ | R5 (XWayland) |
| `ext-idle-notify-v1` | ✓ | ✓ | R4.1 |
| `ext-session-lock-v1` | – (private overlay) | ✓ | R4.1 |
| `ext-workspace-v1` | – (private) | ✗ | **R4.8** |
| `ext-foreign-toplevel-list-v1` | – (private) | ✗ | **R4.8 / R5** |
| `ext-data-control-v1` | ✓ | ✗ | R5 |
| `ext-image-copy-capture-v1` | – | ✗ | R5 |
| `ext-transient-seat-v1` | ✗ | ✗ | R4.3 / R6 |
| `ext-background-effect-v1` | ✓ | ✗ | R6 |
| `wp-fractional-scale-v1` | ✓ | ✗ | R4.5 / R6 |
| `wp-color-management-v1` | ✓ | ✗ | R6 |
| `wp-color-representation-v1` | ✓ | ✗ | R6 |
| `wp-content-type-v1` | ✓ | ✗ | R6 |
| `wp-cursor-shape-v1` | ✓ | ✗ | **R4.7** |
| `wp-drm-lease-v1` | ✓ | ✗ | R6 |
| `wp-fifo-v1` | ✓ | ✗ | R6 |
| `wp-single-pixel-buffer-v1` | ✓ | ✗ | R6 |
| `wp-tearing-control-v1` | ✓ | ✗ | R6 |
| `wp-alpha-modifier-v1` | ✓ | ✗ | R6 |
| `wp-pointer-warp-v1` | ✓ | ✗ | R5 |
| `wp-pointer-constraints-v1` | ✓ | ✗ | R5 |
| `wp-pointer-gestures-v1` | ✓ | ✗ | R5 |
| `wp-relative-pointer-v1` | ✓ | ✗ | R5 |
| `wp-primary-selection-v1` | ✓ | ✗ | R5 |
| `wp-keyboard-shortcuts-inhibit-v1` | ✓ | ✗ | R5 |
| `wp-linux-drm-syncobj-v1` | ✓ | ✗ | R6 |
| `xdpip` (`xx-pip-v1`) | ✓ (private) | ✗ | R6 (private) |

**KDE-private protocols (NOT parity targets).** `org_kde_kwin_*`
(plasma-shell, plasma-window-management, plasma-virtual-desktop, output-device,
output-management, output-order, primary-output, screen-edge, lockscreen-overlay,
idle/timeout, dpms, keystate, fake-input, appmenu, blur, contrast, shadow, slide,
server-decoration[-palette], surface-extension, external-brightness), `zkde_screencast`,
`text-input-v2`, `wl_eglstream_controller`, `frog-color-management`, and the
`xx-` experiments. SDE replaces these roles with standard protocols
(`ext-workspace-v1`, `ext-foreign-toplevel-list-v1`, `ext-session-lock-v1`,
`xdg-activation-v1`, …) plus its minimal `serenity-window`.

**Portal parity.** `xdg-desktop-portal-kde` implements nearly every portal
interface; `xdg-desktop-portal-wlr` + `-gtk` cover only part of that. Reaching
portal parity with Plasma therefore requires an **SDE portal backend** (§10),
starting with **Settings** for R4 (appearance) and growing in R5.

**Exit criterion (added to §7).** SDE advertises and correctly handles the
standard protocols in the table, and third-party clients that rely on KWin's
standard surface (toolkits, `waybar`/`ext-workspace-v1`, `swaylock`,
`wl-clipboard`, portals) work unmodified. The protocol inventory in
[`02-compositor-decision.md`](02-compositor-decision.md) §13 and the per-compositor
**Wayland protocols support table**
(<https://absurdlysuspicious.github.io/wayland-protocols-table/>) are the
tracking references.
