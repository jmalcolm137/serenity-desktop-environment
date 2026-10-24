# SDE Launch Release Checklist

**Purpose:** the gate between “works on the development machine” and “posted to
Hacker News”.
**Status:** in progress · **Owner:** release maintainer · **Last updated:** 2026-10-23

This is deliberately stricter than the normal test suite. A green CI run is
necessary, not sufficient: the install path, packaging metadata and first-login
experience must also be reproducible on a clean machine.

## Progress as of 2026-10-24

- Full suite, smoke, image and budget checks pass on run `40048413024`.
- Framework package built: `serenity-lagom-git r570.04a2577-1`.
- Session package built: `serenity-desktop-environment-git r572.6cbda1c-1`.
- Both packages were extracted into a clean prefix and started headless; Taskbar,
  Desktop and all eight applets mapped without a crash.
- The session package’s `WindowServer` RUNPATH is normalized to
  `/usr/lib/sde/lib`.
- AUR publication is blocked on missing AUR SSH credentials on the maintainer
  machine.
- The machine-level clean install and reboot test is still pending.

## 0. Current blockers

- [ ] `main` is ahead of the installed AUR package. Installed:
      `serenity-desktop-environment-git r559.bafab43-1`; `main` is `b9b9435`.
- [ ] The nightly/full suite is red in `run-wlcs-test.sh`.
- [ ] The AUR package still uses an unpinned `git+…` source and stale
      `pkgver`/`.SRCINFO`.
- [ ] The AUR packages are not published yet. The README therefore documents
      the local `makepkg` path; publish both packages before claiming
      `yay -S`/AUR installation.
- [ ] No clean-machine install has been recorded for the current `main`.
- [ ] `README.md` documents source builds but not the package install path.

Do not announce until every item in **1–5** is checked.

## 1. Version and tag

- [ ] Choose the public version, e.g. `v0.1.0`.
- [ ] Update `README.md` and the release notes with the version and date.
- [ ] Tag the exact commit that was tested:
      `git tag -s v0.1.0 -m "SDE v0.1.0" && git push origin v0.1.0`.
- [ ] Pin the AUR `source=` entries to the tag (or a full commit) so a package
      build is reproducible.
- [ ] Record the framework (`serenity-lagom-git`) revision the release was
      built and tested against.

## 2. Packages

- [ ] Rebuild `serenity-lagom-git` from the release commit of
      `libgui-wayland`.
- [ ] Rebuild `serenity-desktop-environment-git` from the SDE release tag.
- [ ] Regenerate both `.SRCINFO` files:
      `makepkg --printsrcinfo > .SRCINFO`.
- [ ] Confirm `pkgver()` resolves to the release tag/commit, not an older
      package.
- [ ] Confirm the package contains:
      - [ ] `sde-supervise-applet.sh`
      - [ ] `sde-polkit-agent.py` and `sde-auth-dialog`
      - [ ] `sde-lock`
      - [ ] Taskbar, applets, Battery and Wi-Fi
      - [ ] session entry and launcher
- [ ] Sign/checksum release artifacts if publishing binaries outside AUR.
- [ ] Archive the exact built package files used for the launch test.

## 3. Clean-machine test

Use a fresh Arch VM or a machine with no prior SDE install, no
`/usr/lib/sde`, no developer `~/.config/systemd/user/sde-applets.service`, and no
source checkout on the test account.

### Install

- [ ] Install the two release packages through the documented AUR path.
- [ ] Display manager lists **Serenity Desktop Environment**.
- [ ] Log in; no manual applet launching is required.
- [ ] Taskbar, Desktop, WorkspacePicker and all applets appear.
- [ ] Battery is rightmost and Wi-Fi immediately to its left.
- [ ] `pacman -Q serenity-desktop-environment-git` matches the release.
- [ ] Session log has no `VERIFICATION FAILED`, crash or applet supervisor
      restart loop.

### Session

- [ ] Open Terminal, File Manager, Settings and a LibGUI app.
- [ ] Window move, resize, snap, maximize, minimize and restore.
- [ ] Workspaces and Taskbar window list stay in sync.
- [ ] Fonts, theme and wallpaper are correct at scale 1 and HiDPI.
- [ ] XWayland app starts if `xorg-xwayland` is installed.
- [ ] Wi-Fi panel can scan/connect; Battery reports status.
- [ ] Clipboard, notifications, portals and screenshots work.
- [ ] Lock with Super+L; wrong password fails; correct password unlocks.
- [ ] PolicyKit prompt appears, authenticates, and can remember the password
      for the session.
- [ ] No password is written to disk.

### Reboot

- [ ] Reboot once.
- [ ] Log in again; every applet returns without manual intervention.
- [ ] Lock/unlock and Polkit still work.
- [ ] Reboot a second time; behaviour is unchanged.
- [ ] Log out and back in; no duplicate applets or stale processes.

### Failure/recovery

- [ ] Kill an applet; the supervisor restarts it.
- [ ] Kill the Taskbar; the session recovers as designed.
- [ ] Stop the compositor; the session supervisor handles it without a wedged
      machine.
- [ ] `SDE_SAFE_MODE=1` starts a usable fallback session.

## 4. CI and test evidence

- [ ] Push CI green on the release commit.
- [ ] Nightly/full suite green on the release commit.
- [ ] WLCS default suite green; known failures triaged in `docs/32-wlcs.md`.
- [ ] Lock, Polkit, applet layout/supervision, frame-hit, layer, pointer and
      keyboard tests green.
- [ ] Performance and session-memory budgets recorded (not necessarily gated).
- [ ] Attach or link the CI run and local logs in the release notes.

### Current CI blockers

- [ ] Fix `run-wlcs-test.sh` in the full suite. The last scheduled run failed
      before the test binary ran; check the CI image has `boost`/`gtest` and that
      the WLCS build script reports the real failing command.
- [ ] Rebuild the CI image after dependency changes and rerun the full suite.

## 5. Documentation and support

- [ ] README has a **Packages** section with exact install commands.
- [ ] README states the supported platform, currently Arch Linux x86_64.
- [ ] README states **experimental**, not production-ready.
- [ ] Link the user manual, WLCS findings, known gaps and architecture docs.
- [ ] Include an uninstall path and how to return to another desktop session.
- [ ] Add an issue template or a short “reporting a problem” section with the
      session log path and package versions.
- [ ] Prepare FAQ answers:
      - Why not just use SerenityOS?
      - Why Wayland?
      - What hardware is known to work?
      - How do I install it?
      - What is missing?
      - How can I contribute?
- [ ] Check license/third-party notices: SDE, SerenityOS, libgui-wayland,
      wlroots and packaged dependencies.

## 6. Launch assets

- [ ] Final screenshot at a readable resolution.
- [ ] Optional 20–40 second screen recording showing:
      Taskbar menu, applets, Terminal, File Manager, Settings, lock screen.
- [ ] One-paragraph project description.
- [ ] HN title and body prepared (below).
- [ ] A second person available to answer comments for the first few hours.
- [ ] Rollback plan if the post exposes a serious bug: keep the release commit
      tagged, publish a known-issues note, and do not silently rewrite the tag.

## 7. Hacker News post

Title candidate:

> Serenity Desktop Environment: SerenityOS’s UI on Wayland

Body direction:

> SDE runs SerenityOS’s unmodified LibGUI applications on Linux, with a
> wlroots-based Wayland compositor providing the desktop session. It is
> experimental, currently Arch-focused, and the packages build from source.
> I’d especially like feedback on input behavior, protocol conformance, and
> packaging.

- [ ] Do not claim production readiness or full SerenityOS parity.
- [ ] Link the repository, screenshot and install instructions.
- [ ] Mention the largest known limitations up front.
- [ ] Post during a period when the maintainer can respond.

## 8. Sign-off

| Gate | Owner | Evidence | Date |
|---|---|---|---|
| Release tag | | | |
| Framework package | | | |
| Session package | | | |
| Clean-machine install | | | |
| Reboot test | | | |
| Full CI | | | |
| Documentation | | | |
| Launch post | | | |
