# SerenityOS changes carried by the SDE

The SDE builds SerenityOS's Taskbar, Terminal and application suite from the shared
checkout that `libgui-wayland` fetches and patches. A change to those sources lives
in one of three places, decided by one question: **does the change make sense for
SerenityOS on its own, regardless of how the SDE uses it?**

| Where | What belongs there |
|---|---|
| [`jmalcolm137/serenity`](https://github.com/jmalcolm137/serenity), branch `fixes` | Fixes that make sense for SerenityOS in isolation. Each is an ordinary commit on top of the pinned upstream revision and a candidate for an upstream PR. `libgui-wayland/scripts/fetch-serenity.sh` fetches this branch as the base checkout, so both repositories consume these fixes unmodified. |
| `libgui-wayland/patches/` | Changes the port needs to run SerenityOS's userland on a Linux host (the port's own decision). |
| `patches/` (here) | Changes that only exist because of the SDE: SDE behaviour, SDE libraries, SDE build integration. |

A change that only makes sense because of the SDE does not go into the fork, even
if it is small and harmless there. Moving to a newer upstream is a `git rebase` of
the fork's `fixes` branch, after which `fetch-serenity.sh`'s pin moves to the
rebased tip.

## Status of every change

| Change | Was | Now | Why it lives there | Upstream status |
|---|---|---|---|---|
| Taskbar: don't crash painting a quick launch entry with no 16x16 icon | SDE `0007` (a copy was also inside `0001`) | fork `fixes` [`e409d07a0e`](https://github.com/jmalcolm137/serenity/commit/e409d07a0e) | `bitmap_for_size(16)` can return null on SerenityOS too (an app file naming a missing icon, or one without that size). | Not yet submitted |
| Taskbar: drop quick launch entries whose app file is invalid | SDE `0009` | fork `fixes` [`31fdfaad33`](https://github.com/jmalcolm137/serenity/commit/31fdfaad33) | A pinned entry outlives its app on SerenityOS too, and `AppFile::name()` VERIFYs on an invalid file, taking the Taskbar down. Covered by `scripts/run-taskbar-quicklaunch-test.sh`. | Not yet submitted |
| DisplaySettings: offer the current resolution when it is not in the list | SDE `0010` | fork `fixes` [`054963e811`](https://github.com/jmalcolm137/serenity/commit/054963e811) | Without an EDID the fallback list need not contain the mode in use (a VM, a panel's native 2560x1600), and the combo box then reported a resolution the screen was not using. | Not yet submitted |
| Taskbar: resolve an app's executable path before `access()` | SDE `0003` | removed: entirely contained in `0001` | SDE-only. SerenityOS launches app files with `posix_spawn` and no PATH search, so its `access()` check already matches its launch; the SDE maps `/bin/X` to a session app directory. | n/a |
| Terminal: link the SDE system library | SDE `0006` | removed: entirely contained in `0002` | SDE-only (see `0002`). | n/a |
| [`0001`](0001-taskbar-system-menu.patch) Taskbar: the system menu and quick launch for SDE apps | SDE | SDE | SDE behaviour: the SDE's menu layout and app-file resolution against its session app directory. | n/a |
| [`0002`](0002-terminal-shell-environment.patch) Terminal: the shell environment, LibSerenitySystem | SDE | SDE | SDE behaviour: the session's environment and the SDE's host-integration library. | n/a |
| [`0004`](0004-profiler-libserenitysystem.patch) Profiler: link LibSerenitySystem | SDE | SDE | The SDE's host profiling backend; not a SerenityOS concept. | n/a |
| [`0005`](0005-taskbar-window-grouping.patch) Taskbar window grouping, and its LibGUI/IPC half (app id, `taskbar_group_windows`) | SDE | SDE | The IPC change only has a sender in the SDE's compositor. SerenityOS's own WindowServer would need a matching change and has no app-id concept to send, so it is not sensible in isolation as it stands. | Would need a SerenityOS WindowServer change first |
| [`0006`](0006-taskbar-lock-menu.patch) Taskbar: a Lock item, first in the system menu | SDE | SDE | SDE behaviour: the entry locks through `SDE_LOCK_CMD`, which the SDE session exports (and which selects `sde-lock` or a third-party locker). | n/a |
| [`0008`](0008-sde-lagom-targets.patch) The SDE's Lagom targets (the Taskbar, the app suite) | SDE | SDE | SDE build integration: which SerenityOS programs the desktop builds on Lagom. | n/a |
| [`0009`](0009-taskbar-xdg-menu.patch) Taskbar: the XDG system menu (the full freedesktop Menu specification) | SDE | SDE | SDE behaviour: the freedesktop menu data dirs, menu file, `.directory` entries and `.desktop` entries are a host-desktop concept; the menu is selected by `[Taskbar] XDGMenu` and the entries come from the SDE's generator. See [`../docs/36-xdg-desktop-menu.md`](../docs/36-xdg-desktop-menu.md). | n/a |

`libgui-wayland`'s former `0035` (an empty stub left behind when its targets moved
into `0008`) is removed too.

## Checking the stack

`scripts/build.sh` reports `N applied, N already applied, N unverifiable`. On a
fresh checkout the last two must both be 0. "Already applied" means a patch is
redundant (as `0003` and `0006` were, each entirely inside another patch), and
"unverifiable" means it no longer matches the tree. Before this reorganisation the
stack reported three patches as "already applied", which is how the duplicates went
unnoticed and why the order of the stack mattered (`0006` conflicted when applied
after `0002`).
