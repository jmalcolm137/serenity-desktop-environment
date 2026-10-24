# SDE v0.1.0

**Status:** experimental · **Date:** 2026-10-24

Serenity Desktop Environment runs SerenityOS's LibGUI applications on a
wlroots-based Wayland compositor, with the SerenityOS Taskbar, Desktop, applets,
themes and applications as a coherent Linux desktop session.

This is the first public release. It is experimental: keep another desktop
session available, and expect rough edges in hardware coverage, packaging and
Wayland protocol parity.

## Packages

The release uses the split package layout:

- `serenity-lagom-git` — LibGUI/LibGfx, SerenityOS applications, resources and
  framework libraries.
- `serenity-desktop-environment-git` — WindowServer, session services, applets,
  portals and tools.

Install the framework package first, then the session package:

```sh
sha256sum -c SHA256SUMS
sudo pacman -U serenity-lagom-git-0.1.0-1-x86_64.pkg.tar.zst
sudo pacman -U serenity-desktop-environment-git-0.1.0-1-x86_64.pkg.tar.zst
```

The tested target is Arch Linux x86_64. Install a display manager such as SDDM,
log out, and choose **Serenity Desktop Environment** at the login screen.

AUR publication is not currently available, so the package recipes are also kept
in [`Meta/packaging/aur`](https://github.com/jmalcolm137/serenity-desktop-environment/tree/v0.1.0/Meta/packaging/aur)
for local `makepkg` builds.

## What is included

- WindowServer: xdg-shell, layer-shell, xdg-decoration, XWayland, session lock,
  fractional scale, portals and the Serenity window-manager behaviours.
- Taskbar, Desktop, applets, Settings, Terminal, File Manager and the SerenityOS
  application suite.
- Lock screen, PolicyKit agent, Battery and Wi-Fi applets, applet supervision.
- WLCS harness and the SDE test suite; the default WLCS surface, subcompositor,
  keyboard, output and layer-shell groups pass.

## Verification

- CI passed the full suite, smoke tests, image check and performance budgets on
  the release tree.
- A clean Arch Linux cloud VM installed both packages through SDDM autologin and
  rebooted twice. The Taskbar, Desktop and all eight applets returned after each
  boot without manual intervention.
- The package payload was also started headless from a clean extraction prefix;
  Taskbar, Desktop and all eight applets mapped without a crash.
- CI covers the lock screen, PolicyKit, input, layer-shell, frame-hit and
  conformance tests.

Checksums are attached as `SHA256SUMS`.

## Known limitations

- Arch Linux x86_64 is the tested packaging target.
- Hardware coverage is limited; test on your own machine before relying on it.
- Some Wayland conformance and input edge cases remain; see the
  [WLCS notes](https://github.com/jmalcolm137/serenity-desktop-environment/blob/v0.1.0/docs/32-wlcs.md)
  and [known gaps](https://github.com/jmalcolm137/serenity-desktop-environment/blob/v0.1.0/docs/04-known-gaps.md).
- AUR publication is pending.
- The session is experimental and is not a full SerenityOS replacement.

SDE is not affiliated with, endorsed by, or sponsored by the SerenityOS project.
