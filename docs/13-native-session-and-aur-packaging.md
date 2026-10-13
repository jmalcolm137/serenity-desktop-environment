# Native Session & AUR Packaging Plan

**Status:** Draft v0.1 · **Date:** 2026-10-10
**Related:** [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) · [`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md) §1 · [`04-known-gaps.md`](04-known-gaps.md)

This document plans how to run SDE **natively** (DRM/KMS + libinput, no host
compositor underneath) and how to **package it for Arch/EndeavourOS (AUR)** so a
display manager lists it as a session. It is a plan, not an implementation: it
records the package split, the install layout, the packaging traps we have
verified against the current build, and the order to bring it up safely.

> **Scope note.** Packaging is nominally **R6** ("Hardening & packaging",
> [`01-objectives.md`](01-objectives.md) §11) and is not started. This plan pulls
> enough of it forward to satisfy the concrete goal — *install SDE from the AUR on
> EndeavourOS, log out of KDE Plasma, and choose SDE at the login screen* — without
> claiming R6 is done.

---

## 1. Goals and non-goals

**Goal.** On an EndeavourOS machine with `systemd`/`logind` present:

1. Install SDE from locally built AUR packages.
2. Log out of Plasma; SDDM lists **Serenity Desktop Environment** alongside Plasma.
3. Log into SDE → a native DRM/KMS session (Taskbar, Desktop, apps).
4. Log out of SDE back to SDDM; Plasma remains intact.

**Non-goals (for this plan).** Replacing Plasma, an SDE greeter/PAM login (we use
SDDM), publishing to the AUR (build local packages first), and closing the
R4/R5/R6 feature gaps. This plan reuses `logind` opportunistically (the easy path
on EndeavourOS); the *portable* no-logind path is
[`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md).

---

## 2. What has to be installed

Two repos build the product: `libgui-wayland` (Serenity userland: LibWindowServer
+ Lagom libs + the apps) and **this repo** (the compositor, session and services).
Verified shape of the current build:

- **Compositor** links `libwlroots-0.20.so` (**pinned** wlroots `0.20.2`, not
  Arch's) plus ~15 `liblagom-*.so.0` and system libs.
- **Lagom**: ~70 shared libs (~305 MB unstripped) in `build/lagom/lib`.
- **Apps**: ~99 binaries (~74 MB) in `build/lagom/bin`, with
  `RUNPATH=$ORIGIN:$ORIGIN/../lib` (packaging-friendly).
- **Resources**: `serenity/Base/res` (~29 MB): fonts, themes, icons, wallpapers,
  color-schemes, `LaunchServer.ini`.
- **Services/tools**: `LaunchServer`, `ImageDecoder`, `sde-idle`.

---

## 3. Package set (recommended)

| Package | Contents | Runtime deps | Build cost |
|---|---|---|---|
| `wlroots0.20` | pinned `wlroots 0.20.2` → `libwlroots-0.20.so` + `wlroots-0.20.pc` | wayland, libinput, libdrm, libseat, pixman, xcb-* | moderate |
| `serenity-lagom-git` | `liblagom-*.so`, the apps, `Base/res`, **and** the Serenity headers/Lagom build tree (see §5 trap 6) | build deps of Serenity Lagom | **large** (Serenity) |
| `serenity-desktop-environment-git` | `WindowServer`, `LaunchServer`, `ImageDecoder`, `sde-idle`, session scripts, `sde.desktop` | `serenity-lagom-git`, `wlroots0.20` | small |

> Arch's stock `wlroots` cannot be used: the compositor's CMake requires
> `pkg_check_modules(wlroots-0.20)`, so the pinned 0.20.x must be packaged (or
> vendored/static inside the SDE package). Prefer a separate `wlroots0.20` so it
> is reusable and clearly version-scoped.

---

## 4. Install layout: a private prefix

Install everything SDE-specific under **`/usr/lib/sde/`** so nothing pollutes the
system and generic app names (`FileManager`, `Taskbar`) cannot clash:

```
/usr/lib/sde/bin/            WindowServer, LaunchServer, ImageDecoder,
                             sde-idle, the Serenity apps (Taskbar, …)
/usr/lib/sde/lib/            liblagom-*.so*
/usr/lib/sde/share/res/      Base/res  (fonts, themes, icons, wallpapers, …)
/usr/lib/sde/share/sde/      session scripts (sde-session.sh, sde-power.sh, …)
/usr/share/wayland-sessions/sde.desktop
```

Rationale: the apps' `RUNPATH=$ORIGIN:$ORIGIN/../lib` resolves automatically when
apps sit in `…/sde/bin` and libs in `…/sde/lib`; and a single
`LD_LIBRARY_PATH=/usr/lib/sde/lib` in the launcher covers the compositor too.

---

## 5. Packaging traps (verified against the current build)

1. **Compositor RUNPATH points at the build tree.** `WindowServer` carries
   `RUNPATH=…/libgui-wayland/build/lagom/lib:…/third_party/wlroots/install/lib`.
   Fix in the PKGBUILD with `patchelf --set-rpath /usr/lib/sde/lib` (and strip).
2. **wlroots version pin.** Separate `wlroots0.20` package (or static vendoring);
   never rely on the distro `wlroots`.
3. **Strip everything.** Lagom libs are ~305 MB unstripped and debug-heavy;
   stripping is a large size win.
4. **Resource root.** Set `SERENITY_RES_ROOT=/usr/lib/sde/share/res`.
5. **App resolution.** Set `SDE_APP_BIN_DIR=/usr/lib/sde/bin` so the shell and the
   Serenity "launch `/bin/X`" paths find the apps.
6. **The compositor builds against the Serenity *source + Lagom build tree*, not
   just installed libs.** `scripts/build-window-server.sh` requires `SERENITY_SRC`
   and `SERENITY_BUILD_DIR` (the `libgui-wayland` checkout and its `build/lagom`).
   So `serenity-lagom-git` must either expose those (headers + build dir under a
   known prefix) or `serenity-desktop-environment-git` must carry the
   `libgui-wayland` checkout as a `makedepends`. This is the main cross-package
   coupling to design early.
7. **Session PID / logind.** SDDM launches the session command; the top process
   should end up being the compositor (or a wrapper that `exec`s it). The
   supervisor already **stops the compositor and services on SIGTERM** (commit
   `eaa7929`), which is what logind sends on logout.
8. **Pinned Serenity revision.** The Lagom build must use the exact Serenity
   revision `libgui-wayland` pins and apply its `patches/` (see
   `scripts/fetch-serenity.sh`), or the compositor's link/host-seam assumptions
   break.

---

## 6. Session entry and wrapper

`/usr/share/wayland-sessions/sde.desktop` (SDDM lists any `wayland-sessions`
entry):

```ini
[Desktop Entry]
Name=Serenity Desktop Environment
Comment=The SerenityOS desktop experience on Wayland
Exec=/usr/lib/sde/bin/sde-session
Type=Application
DesktopNames=SDE
```

`/usr/lib/sde/bin/sde-session` (thin wrapper; the supervisor does the real work):

```sh
#!/bin/sh
export SERENITY_RES_ROOT=/usr/lib/sde/share/res
export SDE_APP_BIN_DIR=/usr/lib/sde/bin
export LD_LIBRARY_PATH=/usr/lib/sde/lib
export XDG_CURRENT_DESKTOP=SDE
export SDE_LOCK_CMD=swaylock            # if installed
exec /usr/lib/sde/share/sde/sde-session.sh
```

The supervisor (`scripts/sde-session.sh`) already sets `XDG_SESSION_TYPE`,
`XDG_CURRENT_DESKTOP`, `WAYLAND_DISPLAY=sde`, runs XDG autostart, starts/stops
session services, and restarts the compositor on fault. The compositor runs as
`WindowServer -n sde -s "Taskbar & FileManager -d & TextEditor"`.

---

## 7. Seat / logind in a DM session

Launched by SDDM, the session is registered with **`systemd-logind`**, so
`wlr_backend_autocreate` + **libseat** (`logind` backend) grants DRM master and
handles VT switching — no extra work. This is exactly the "opportunistic logind"
case in [`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md);
the portable `seatd`/`builtin` path is the fallback for no-logind machines, not
required here.

---

## 8. Bring-up order (validate before packaging)

1. **Nested** (already works): `scripts/start-sde-nested.sh` — proves the shell
   and apps.
2. **Native from a TTY** (no packaging): from a spare VT (`Ctrl+Alt+F3`), run
   `scripts/start-sde-session.sh`. With `WAYLAND_DISPLAY`/`DISPLAY` unset this
   selects DRM+libinput and libseat grants DRM master. **If this does not come
   up, no PKGBUILD will save you** — fix the compositor first.
3. **Local packages**, then install with `pacman -U`.
4. **From SDDM**: log out of Plasma, pick *Serenity Desktop Environment*.

Keep `Ctrl+Alt+F2` as the escape hatch throughout; if SDE hangs, switch VT and
`pkill WindowServer`.

---

## 9. PKGBUILD sketches (outline)

**`wlroots0.20`** — source `wlroots` `0.20.2`; `meson` build; install the shared
lib and `wlroots-0.20.pc` under `/usr` (so other packages' `pkg-config` finds it).

**`serenity-lagom-git`** — sources: `libgui-wayland` (git) + its pinned
`serenity` revision + `patches/`. Build via the project's
`scripts/fetch-serenity.sh` + `scripts/build-libgui.sh` (CMake/Ninja, Lagom).
`package()` copies `build/lagom/lib` → `/usr/lib/sde/lib` (stripped),
`build/lagom/bin` → `/usr/lib/sde/bin` (stripped), `serenity/Base/res` →
`/usr/lib/sde/share/res`, and (per trap 6) the Serenity headers + build tree to a
known prefix for the compositor build.

**`serenity-desktop-environment-git`** — source: this repo. `makedepends` include
`serenity-lagom-git` (for `SERENITY_SRC`/`SERENITY_BUILD_DIR`) and `wlroots0.20`.
`build()` runs `scripts/build-window-server.sh` plus the `LaunchServer`/
`ImageDecoder`/tools builds. `package()` installs `WindowServer`, the services and
`sde-idle` to `/usr/lib/sde/bin`, the session scripts to `/usr/lib/sde/share/sde`,
the `sde-session` wrapper to `/usr/lib/sde/bin`, and
`/usr/share/wayland-sessions/sde.desktop`; then `patchelf`s the compositor's
RUNPATH to `/usr/lib/sde/lib`.

---

## 10. Risks and caveats

- **Build cost.** The Serenity Lagom build is large (tens of GB, long). One-time
  per update, but real.
- **Early session (R4 in progress).** No XWayland, no `xdg-desktop-portal`, no
  IME/accessibility, coarse multi-output/hotplug. "Try it", not a Plasma
  replacement.
- **Rolling-release drift.** A pinned wlroots `0.20.2` may need small patches
  against newer `wayland-protocols`/`libinput`.
- **Toolchain.** Lagom here is built as C++26; needs a recent GCC/Clang (Arch's is
  fine).
- **Don't remove Plasma.** Keep a second session and a VT escape hatch.

---

## 11. Open decisions

| # | Decision | Options | Leaning |
|---|---|---|---|
| P1 | Prefix | `/usr/lib/sde` vs system paths | Private prefix (**§4**) |
| P2 | wlroots | separate `wlroots0.20` vs vendored/static | Separate package |
| P3 | Lagom exposure to the compositor build | install headers+build tree vs `makedepends` on the checkout | Install headers+build tree under the prefix (trap 6) |
| P4 | Source of truth for the Serenity revision | `libgui-wayland` pin vs SDE pin | Follow `libgui-wayland`'s pin |
| P5 | Distribute | local `pacman -U` now vs publish AUR later | Local packages first |

---

## 12. References

- [`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md)
  — session env, seat (`libseat`) and the no-logind path.
- [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) — R4 milestones
  (R4.2 supervisor/services are reused here).
- [`08-compositor-gaps.md`](08-compositor-gaps.md) §1 — session/seat contract.
- `libgui-wayland` — `DESIGN.md`, `scripts/build-libgui.sh`,
  `scripts/fetch-serenity.sh`.
- Arch packaging — `PKGBUILD(5)`, `makepkg(8)`, `patchelf`, SDDM
  `wayland-sessions`.

---

## Appendix A — HiDPI and fractional output scale

**Status:** implemented · `WindowServer/src/main.cpp`, `docs/11` §11.

Serenity's LibGUI renders on the CPU and its fonts are fixed bitmaps, so a
high-DPI panel is made usable by **scaling the output** — integer by default,
fractional supported:

- **Auto rule** (`auto_output_scale`): 2x when the panel's true DPI is `>= 144`
  (from `phys_width`), else 1x; when the panel omits its physical size (common on
  eDP) it falls back to `width >= 2560 && height >= 1600`.
- **Override, fractional included:** `[Output] DefaultScale` in
  `WindowServer.ini` (0 = auto; `0.5`–`4.0`, e.g. `1.5`) or the `SDE_OUTPUT_SCALE`
  environment variable. `sde-config set Output DefaultScale 1.5` writes it, and a
  **live edit re-scales the outputs immediately** (`Config::watch` →
  `apply_output_scale`). Precedence: `SDE_OUTPUT_SCALE` > `[Output] DefaultScale`
  > the saved `screenlayout.ini` scale > auto.
- **Protocol:** `wp-fractional-scale-v1` is served. `wlr_scene` assigns each
  mapped surface the output's preferred scale, so every client (native or
  Serenity) learns the exact scale; `wl_output.scale` is the integer ceiling.
- **Client rendering (host M8) — done.** The libgui-wayland shim reads the output
  scale and rasters the backing store at it (`Window::set_backing_store_scale`),
  presenting a physical buffer with `wl_surface.set_buffer_scale`, so client text
  is rendered at native resolution, not upscaled. At a fractional scale the client
  supersamples at `ceil(scale)` (e.g. 2x for 1.5) and the compositor downscales
  1:1-crisply.
- **Crisp compositor chrome.** Compositor-drawn chrome (frames, overlays, the
  Alt-Tab switcher, the geometry HUD, menus) is rendered at an integer supersample
  of the output scale into a physical-resolution bitmap that the scene presents at
  the logical size.
- **Filtering.** Integer scales use `WLR_SCALE_FILTER_NEAREST` (a pixel-doubling
  no-op at 1x, crisp at 2x); a fractional scale uses bilinear.
- **Logical sizing:** the compositor sizes its own surfaces from
  `wlr_output_effective_resolution()` (logical), and a saved size with no matching
  mode requests a **custom mode** (so a headless capture can be, e.g., 2560x1440).
- **Capturing a HiDPI scene:** `scripts/run-desktop-scene.sh --headless --scale 2
  --size 1280x720 --screenshot out.png` lays the scene out in 1280x720 *logical*
  coordinates and captures a 2560x1440 *physical* image; `--scale 1.5` captures
  1920x1080.

**DisplaySettings.** The Monitor tab is now **live**. **Resolution** and
**Position** (X/Y) come from the layout and, together with the **Display scale**
control (1x / 1.25x / 1.5x / 2x), take effect when you press **Apply**: the shim
writes `screenlayout.ini` (mode + position) and `[Output] DefaultScale` (scale),
and the compositor applies both live (it watches the files). We deliberately skip
SerenityOS's "Do you want to keep the new screen layout?" revert dialog —
`SettingsWindow` applies every tab, so a modal there would freeze the window. The
scale is also sent as v120 over a `set_output_scale` WindowServer-IPC method
(libgui-wayland patch `0042`) because Serenity's `ScreenLayout` `ScaleFactor` is an
integer and cannot carry `1.5`; the Position spin boxes are meaningful with
multiple outputs (`normalize()` keeps a single screen at 0,0).
