# SDE v0.1.0-rc1

**Status:** experimental release candidate · **Date:** 2026-10-24

Serenity Desktop Environment runs SerenityOS's LibGUI applications on a
wlroots-based Wayland compositor, with the SerenityOS Taskbar, Desktop, applets,
themes and applications as a coherent Linux desktop session.

This is a release candidate. Keep another desktop session available, and expect
rough edges in hardware coverage, packaging and Wayland protocol parity.

## Packages

The release uses the split package layout:

- `serenity-lagom-git` — LibGUI/LibGfx, SerenityOS applications, resources and
  framework libraries.
- `serenity-desktop-environment-git` — WindowServer, session services, applets,
  portals and tools.

Install the framework package first, then the session package:

```sh
sudo pacman -U serenity-lagom-git-r570.04a2577-1-x86_64.pkg.tar.zst
sudo pacman -U serenity-desktop-environment-git-r572.6cbda1c-1-x86_64.pkg.tar.zst
```

Install a display manager such as SDDM, log out, and choose **Serenity Desktop
Environment** at the login screen.

AUR registration is not currently open, so the package recipes are also kept in
[`Meta/packaging/aur`](../Meta/packaging/aur) for local `makepkg` builds.

## What is included

- WindowServer: xdg-shell, layer-shell, xdg-decoration, XWayland, session lock,
  fractional scale, portals and the Serenity window-manager behaviours.
- Taskbar, Desktop, applets, Settings, Terminal, File Manager and the SerenityOS
  application suite.
- Lock screen, PolicyKit agent, Battery and Wi-Fi applets, applet supervision.
- WLCS harness and the SDE test suite; the default WLCS surface, subcompositor,
  keyboard, output and layer-shell groups pass.

## Known limitations

- Arch Linux x86_64 is the tested packaging target.
- Hardware coverage is limited; test on your own machine before relying on it.
- Some Wayland conformance and input edge cases remain; see
  [`docs/32-wlcs.md`](32-wlcs.md).
- AUR publication is pending.
- Hacker News announcement is intentionally held until a clean-machine install
  and reboot test has been recorded.

## Verification

The release candidate was built from the `main` branch after the full CI run
including the full suite and WLCS. The extracted package payload starts a
headless session with the Taskbar, Desktop and all eight applets.

Checksums are attached as `SHA256SUMS`.
