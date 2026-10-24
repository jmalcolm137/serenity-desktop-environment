# SDE v0.1.0 Release Checklist

**Status:** released · **Owner:** release maintainer · **Last updated:** 2026-10-24

This checklist records the evidence for the first public SDE release. The
release is intentionally limited to Arch Linux x86_64 and is marked experimental.

- Release: <https://github.com/jmalcolm137/serenity-desktop-environment/releases/tag/v0.1.0>
- Release commit: `751efc9697aa915b927433f127ff9d84ce5b8e47`
- CI: <https://github.com/jmalcolm137/serenity-desktop-environment/actions/runs/40108867168>

## Tag and artifacts

- [x] Final version selected: `v0.1.0`.
- [x] Final tag created and pushed.
- [x] Framework package: `serenity-lagom-git 0.1.0-1`.
- [x] Session package: `serenity-desktop-environment-git 0.1.0-1`.
- [x] Package sources pinned to the release tag and tested framework revision.
- [x] Framework package contains the supervisor, Polkit agent, lock screen,
      Taskbar, applets, Battery and Wi-Fi.
- [x] Session entry and launcher installed.
- [x] `WindowServer` RUNPATH normalized to `/usr/lib/sde/lib`.
- [x] Both packages install a license file.
- [x] `SHA256SUMS` generated and verified against the attached assets.
- [x] AUR publication is unavailable; the release uses GitHub package assets and
      the local `makepkg` recipes.

## CI evidence

- [x] Full suite, smoke suite, image check and budgets pass on the release
      commit.
- [x] WLCS default suite and the compositor input/layer/frame-hit tests pass.
- [x] Lock, Polkit, applet layout/supervision, pointer and keyboard tests pass.

## Clean-machine test

- [x] A clean Arch Linux cloud VM installed both packages.
- [x] SDDM autologin starts the SDE Wayland session.
- [x] Taskbar, Desktop, WorkspacePicker, Battery, Wi-Fi and all configured
      applets map without manual launching.
- [x] Package versions match 0.1.0.
- [x] Session log has no `VERIFICATION FAILED`, crash or supervisor restart loop.
- [x] Rebooting twice reproduces the same working session.
- [x] Killing an applet is recovered by the supervisor.
- [x] All packaged executables resolve their shared libraries.

Interactive checks that are covered by the full suite rather than repeated in the
VM: window input and geometry, lock/PAM, PolicyKit, layer-shell and XWayland
protocol paths.

## Documentation

- [x] README documents the release-package install path.
- [x] README documents source builds and the local package recipes.
- [x] README states the supported platform and experimental status.
- [x] README includes uninstall and problem-reporting guidance.
- [x] Release notes use absolute links.
- [x] Release notes include known limitations and verification evidence.
- [x] Repository topics and the issue template are configured.
- [ ] Repository social preview is still a manual GitHub settings step.

## Sign-off

| Gate | Evidence | Date |
|---|---|---|
| Release tag | `v0.1.0` at `751efc9` | 2026-10-24 |
| Framework package | `serenity-lagom-git 0.1.0-1` | 2026-10-24 |
| Session package | `serenity-desktop-environment-git 0.1.0-1` | 2026-10-24 |
| Clean-machine install | fresh Arch cloud VM | 2026-10-24 |
| Reboot test | two clean reboots | 2026-10-24 |
| Full CI | run 40108867168 | 2026-10-24 |
| Documentation | README, release notes, checklist | 2026-10-24 |
