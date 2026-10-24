# SDE v0.1.0 Release Checklist

**Status:** release preparation · **Owner:** release maintainer · **Last updated:** 2026-10-24

This checklist records the evidence for the first public SDE release. The
release is intentionally limited to Arch Linux x86_64 and is marked experimental.

## Release state

- [x] Final version selected: `v0.1.0`.
- [x] Framework package: `serenity-lagom-git 0.1.0-1`.
- [x] Session package: `serenity-desktop-environment-git 0.1.0-1`.
- [x] Package sources pinned to the release tag and framework revision.
- [x] Framework package contains the supervisor, Polkit agent, lock screen,
      Taskbar, applets, Battery and Wi-Fi.
- [x] Session entry and launcher installed.
- [x] `WindowServer` RUNPATH normalized to `/usr/lib/sde/lib`.
- [x] Both packages install a license file.
- [x] A clean Arch cloud VM installed the packages through SDDM autologin and
      rebooted twice; Taskbar, Desktop and all eight applets returned.
- [x] Extracted package payload started headless; Taskbar, Desktop and all eight
      applets mapped without a crash.
- [x] AUR publication is unavailable; the release uses GitHub package assets and
      the local `makepkg` recipes.

## Tag and artifacts

- [ ] Tag the tested commit as `v0.1.0` and push the tag.
- [ ] Build both packages from the tag with `SDE_RELEASE_VERSION=0.1.0`.
- [ ] Generate `SHA256SUMS` and attach both packages plus the checksum file.
- [ ] Confirm the release assets match the tested package files.

## CI evidence

- [ ] Full CI, smoke suite and budgets pass on the release commit.
- [ ] WLCS default suite passes; known failures are documented.
- [ ] Lock, Polkit, applet layout/supervision, frame-hit, layer, pointer and
      keyboard tests pass.

## Clean-machine test

- [ ] Display manager lists **Serenity Desktop Environment**.
- [ ] Log in; no manual applet launching is required.
- [ ] Taskbar, Desktop, WorkspacePicker and all applets appear.
- [ ] Battery is rightmost and Wi-Fi immediately to its left.
- [ ] Package versions match 0.1.0.
- [ ] Session log has no `VERIFICATION FAILED`, crash or supervisor restart loop.
- [ ] Terminal, File Manager, Settings and a LibGUI application start.
- [ ] Window move, resize, snap, maximize, minimize and restore work.
- [ ] XWayland starts when `xorg-xwayland` is installed.
- [ ] Lock screen rejects a wrong password and unlocks with the correct one.
- [ ] PolicyKit authenticates and can remember the password for the session.
- [ ] Reboot twice; applets return without intervention.
- [ ] Log out and back in; no duplicate applets or stale processes.
- [ ] Killing an applet is recovered by the supervisor.
- [ ] `SDE_SAFE_MODE=1` starts a usable fallback session.

## Documentation

- [x] README documents the release-package install path.
- [x] README documents source builds and the local package recipes.
- [x] README states the supported platform and experimental status.
- [x] README includes uninstall and problem-reporting guidance.
- [x] Release notes use absolute links.
- [x] Release notes include known limitations and verification evidence.
- [x] Documentation contains no announcement copy.

## Public metadata

- [ ] Add repository topics.
- [ ] Add the issue template.
- [ ] Add or refresh the repository social preview.

## Sign-off

| Gate | Evidence | Date |
|---|---|---|
| Release tag | | |
| Framework package | | |
| Session package | | |
| Clean-machine install | | |
| Reboot test | | |
| Full CI | | |
| Documentation | | |
