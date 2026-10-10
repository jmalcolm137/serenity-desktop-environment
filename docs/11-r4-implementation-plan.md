# Serenity Desktop Environment — R4 Implementation Plan

**Status:** Draft v0.1 · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) §11 · [`04-known-gaps.md`](04-known-gaps.md) · [`07-systemserver-roles.md`](07-systemserver-roles.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md) §1, §4 · [`09-r3-implementation-plan.md`](09-r3-implementation-plan.md) §5

R0–R3 are done (see [`09-r3-implementation-plan.md`](09-r3-implementation-plan.md)).
This document is the execution plan for **R4 — Session & settings**. It owns both
scope and sequencing for R4 (unlike R3's `05`/`09` split), and it breaks R4 into
**additional milestones** so the work is ordered and testable.

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

## 2. Additional milestones

R4 is delivered as **R4.1–R4.9**. Order is dependency-driven (see §3).

| # | Milestone | Outcome | Depends on |
|---|---|---|---|
| **R4.1** | **Session safety net** | Idle detection, idle inhibit, session lock and DPMS/dim; a lock client *(compositor side landed; lock UI/auth outstanding)* | — |
| **R4.2** | **Session lifecycle & supervisor** | Session start/autostart, logout/reboot/shutdown, compositor restart + shell relaunch | R4.1 |
| **R4.3** | **logind / systemd integration** | `XDG_SESSION_TYPE`, `WAYLAND_DISPLAY`, D-Bus activation env, `graphical-session.target`, VT switch | R4.2 |
| **R4.4** | **Compositor config & live reload** | `WindowServer.ini` schema (keybindings, focus, snap, workspaces, effects, outputs) + validated live reload | — |
| **R4.5** | **Display / output settings** | Mode/scale/rotation/arrangement backend + working Monitor tab | R4.4 |
| **R4.6** | **Input settings & keymaps** | libinput device config + keyboard layouts/keymap + MouseSettings | R4.4 |
| **R4.7** | **Appearance, effects & fonts** | Session font broker (scalable fonts, working zoom) + Effects + cursor settings | R4.4 |
| **R4.8** | **Workspaces settings tab** | Workspaces tab drives the compositor grid, persisted | R4.4 |
| **R4.9** | **Applications enablement** | Terminal, SystemMonitor, CrashReporter/Debugger feasibility | R4.1–R4.3 |

### R4.1 — Session safety net

The headline R4 mechanism. Compositor-side (wlroots 0.20 provides the pieces):
`ext-idle-notify-v1` (`wlr_idle_notifier_v1`), `zwp_idle_inhibit_v1`
(`wlr_idle_inhibit_manager_v1`), `ext-session-lock-v1`
(`wlr_session_lock_manager_v1`) and DPMS (`wlr_output_power_manager_v1`;
plus dim). Wire `wlr_idle_notifier_v1_notify_activity()` on every input event and
`wlr_idle_notifier_v1_set_inhibited()` while an inhibitor is visible.

The compositor implements the **`ext-session-lock-v1` contract**: one lock at a
time; a lock surface configured per output (including hotplug while locked);
`locked` once every output is covered; no input or normal surfaces shown while
locked; `unlock` tears the lock down. SerenityOS ships **no lock screen**, so R4.1
also delivers a **lock client** (decide in §4: a small SDE client vs an external
lock client).

- **Exit test:** `scripts/run-idle-test.sh` — a scripted `ext-idle-notify`
  client sees a timeout fire, an idle-inhibit client suppresses it, and a DPMS
  request powers an output off/on; `scripts/run-lock-test.sh` — a scripted lock
  client locks, covers every output and unlocks.

**Landed (compositor side).** `WindowServer` now creates the idle-notifier,
idle-inhibit, output-power and session-lock managers; notifies activity on key,
pointer, button and axis input; suppresses idle while inhibited; turns outputs
off/on; and implements the lock contract (per-output lock surfaces, `locked`
when all outputs are covered, input routed to the lock surface while locked).
`scripts/run-idle-test.sh` and `scripts/run-lock-test.sh` pass.

**Remaining for R4.1:** a real **lock client** with authentication (PAM) and a
Serenity-styled lock screen (the test's black-surface client proves feasibility);
lock-surface handling across **hotplug / scale**; and **dim/screen-blank** before
lock. The lock-client decision (§4) is now "write a small SDE client", since
SerenityOS has none and the protocol path is proven.

### R4.2 — Session lifecycle & supervisor

- **Autostart:** a session init that reads an autostart list and launches the
  shell (Taskbar, Desktop) and `~/.config/sde/autostart` entries; replaces the
  hand-rolled startup command in `scripts/start-sde-session.sh`.
- **Power:** Log out / Reboot / Shut down from the Taskbar system menu
  (`ShutdownDialog` spawns `/bin/shutdown`, `/bin/reboot`, `/bin/logout` — map
  these to logind/session actions, see R4.3) instead of failing on host binaries.
- **Supervisor:** wrap the compositor; on exit/fault, restart it and relaunch the
  shell, losing as little session state as possible (complementary to R6's safe
  mode).

- **Exit test:** `scripts/run-session-test.sh` — autostart entries launch;
  power actions are routed (mocked) and asserted; the supervisor restarts a
  killed compositor.

### R4.3 — logind / systemd integration

Export `XDG_SESSION_TYPE=wayland`, `WAYLAND_DISPLAY`, `XDG_CURRENT_DESKTOP`, and
push them to D-Bus (`dbus-update-activation-environment`) so portals and apps see
a Wayland session; order under `graphical-session.target`; VT switching and DRM
master handover via `libseat` (documented, not re-implemented).

- **Exit test:** scripted env assertions; manual VT switch on real hardware.

### R4.4 — Compositor configuration & live reload

Extend the R3 `WindowServer.ini` (`[Workspaces]`, `[Applet]`) into a documented
schema for **keybindings, focus policy, snap/tile behaviour, workspace grid,
effects, output defaults**, with validation and live reload, plus a
`scripts/sde-config` helper. This is the seam R4.5–R4.8 write through.

- **Exit test:** changing a keybinding / grid / effect in the file takes effect
  live; invalid config is rejected with a clear error.

### R4.5 — Display / output settings

Implement the WindowServer protocol `get_screen_layout` / `set_screen_layout`
(previously a single-output stub) against `wlr-output-management`, so
DisplaySettings ▸ **Monitor** works: mode, scale, rotation, arrangement,
primary; persisted. Reasonable limits on headless/nested backends.

- **Exit test:** `scripts/run-output-test.sh` — a scripted layout change
  reconfigures the (headless) output; manual on DRM.

### R4.6 — Input settings & keymaps

`libinput` device configuration (accel profile/speed, tap-to-click, natural
scroll, button mapping, disable-while-typing, left-handed) with persistence, and
real keyboard management: `set_keymap`, multiple layouts, per-device layout.
Enable `MouseSettings`.

- **Exit test:** `scripts/run-input-settings-test.sh` asserts the config is
  applied to a (virtual) device; manual for real hardware.

### R4.7 — Appearance, effects & fonts

Close the R3 font deferral ([`10-font-parity-decision.md`](10-font-parity-decision.md)):
a **session font broker** (like the theme broker) so `set_system_fonts` applies a
chosen family/size to every client + the compositor, with a scalable default so
**zoom works**; wire DisplaySettings ▸ **Fonts**. Also Effects (animations,
shadows, tooltip shadow, window geometry) via `SystemEffects` live update, and
cursor settings.

- **Exit test:** `scripts/run-fonts-test.sh` — a system-font change applies and
  TextEditor zoom changes the size (update `libwm-font-parity-test`);
  `scripts/run-effects-test.sh` — an effect toggles live.

### R4.8 — Workspaces settings tab

Connect DisplaySettings ▸ **Workspaces** to the compositor grid (`[Workspaces]`),
applied live and persisted.

- **Exit test:** apply a grid change; the workspace picker and switching match.

### R4.9 — Applications enablement

`Terminal` runs; make **SystemMonitor** work (`/proc` mapping), and assess
`CrashReporter` / `Debugger` feasibility (both may need shims). Deliver what is
reasonable; record what is not.

- **Exit test:** the app launches in the session and shows real data.

## 3. Sequencing & critical path

```
 R4.1 safety net ──▶ R4.2 lifecycle/supervisor ──▶ R4.3 logind/systemd
      │                                                 │
      └──────────────────┬──────────────────────────────┘
                         ▼
 R4.4 compositor config ─┬─▶ R4.5 display/output
                         ├─▶ R4.6 input/keymaps
                         ├─▶ R4.7 appearance/effects/fonts
                         └─▶ R4.8 workspaces
                                          R4.9 apps (independent; after R4.1–R4.3)
```

**Start with R4.1** (highest value, self-contained, protects the session), then
**R4.4** (the config seam), then the settings backends. R4.9 can run alongside
once the session is safe and startable.

## 4. De-risking spikes (time-boxed, before committing)

| Spike | Question | Decision it unblocks |
|---|---|---|
| **Lock client** | Does SerenityOS have a lock screen we can port, or do we write a minimal SDE lock client, or use an external one for R4? | R4.1 lock client — **resolved**: SerenityOS has none; write a small SDE client (the protocol path is proven by `tools/lock-test.c`) |
| **Restart semantics** | What can a supervisor preserve across a compositor restart (window list, placement, apps)? | R4.2 scope |
| **Output management on nested vs DRM** | How much of the Monitor tab is meaningful headless/nested vs real hardware? | R4.5 exit test |
| **Font broker** | Broker file shape and relayout trigger for a live system-font change | R4.7 |
| **logind availability** | Is logind present in the target environments, and what is the fallback? | R4.2/R4.3 |

## 5. Testing strategy

- **Compositor behaviours** (idle, inhibit, lock, DPMS, output, input, effects)
  get scripted clients under the headless backend, asserting compositor log
  lines and, where useful, state dumps — the pattern R3 established
  (`run-input-test.sh`, `run-applet-test.sh`, `run-tooltip-test.sh`).
- **Settings round-trips** (font broker, output layout, input config) assert the
  state is written, applied, and persists across a restart.
- **Session/power/VT/PAM/logind** are manual on real hardware.

## 6. Exit criteria (R4 definition of done)

R4 is done when:

1. the session can be **locked** and **idles** to lock/DPMS, with idle inhibition
   honoured;
2. the session **starts, autostarts, logs out and powers off/reboots**
   correctly, and a compositor fault is **recovered** by the supervisor;
3. the session is a proper Wayland session for portals/apps (env + target);
4. DisplaySettings is **functional end to end** — Monitor, Fonts (with working
   zoom), Input, Appearance/Effects, Workspaces — driven by a documented,
   live-reloadable config;
5. `Terminal` and (at least) `SystemMonitor` run;
6. every scriptable behaviour has a headless test, and the R1–R3 suites still
   pass.

## 7. Risks and decisions

| Risk / decision | Owner | Note |
|---|---|---|
| No SerenityOS lock screen | R4.1 | Decide SDE lock client vs external; spike first |
| Lock needs authentication (PAM) | R4.1 | Start with a minimal lock; PAM is a follow-on |
| logind/systemd may be absent | R4.2/R4.3 | Document a scripted fallback path |
| `ext-session-lock-v1` on hotplug/scale | R4.1 | Must cover every output, including new ones |
| Output config limited headless | R4.5 | Nested/DRM split in the exit test |
| Font change requires live relayout | R4.7 | Cross-repo (`libgui-wayland`); broker like the theme |
| `CrashReporter`/`Debugger` may need shims | R4.9 | Feasibility-assess; drop if unreasonable |

## 8. Out of scope for R4

R5/R6 breadth — XWayland, portals, IME/accessibility, HiDPI/fractional scaling,
multi-GPU, color management, packaging, CI matrix, sanitizers/fuzzing — remains
where [`08-compositor-gaps.md`](08-compositor-gaps.md) assigns it. Do not pull
R5/R6 work into R4.
