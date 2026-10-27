# XDG user directories

**Status:** implemented · **Related:** [`14-r4.4-config.md`](14-r4.4-config.md),
[`04-known-gaps.md`](04-known-gaps.md)

SDE follows the freedesktop **XDG User Directories** specification: the standard
user directories (Desktop, Downloads, Templates, Public, Documents, Music,
Pictures, Videos) are read from

```
$XDG_CONFIG_HOME/user-dirs.dirs        # default: ~/.config/user-dirs.dirs
```

instead of assuming `$HOME/<English name>`.

## Resolution order

For every directory, `Core::StandardPaths` resolves in this order:

1. the environment variable (`XDG_DESKTOP_DIR`, `XDG_DOWNLOAD_DIR`, …) — kept
   for scripts, tests and embedding;
2. the key in `user-dirs.dirs` (values are shell-style strings, with `"…"`/`'…'`
   quotes stripped and `$HOME`/`${HOME}` expanded);
3. the built-in `$HOME/<Name>` fallback.

A configured value of `$HOME` means "disabled", as the spec defines; the
directory resolves to the home directory and is not created by the updater.

The file is read **once per process** (a thread-safe function-local static), so
changing it requires restarting the application. This matches how toolkits
usually consume it; the session runs the updater before applications start.

## Framework support

`libgui-wayland` patch `0072-libcore-xdg-user-dirs.patch` adds the parser to
`LibCore/StandardPaths.cpp` and the `templates_directory()` /
`public_share_directory()` accessors to `StandardPaths.h`. Existing consumers
therefore follow the file without changes, e.g.:

- `LibGUI/CommonLocationsProvider` (the FileManager/FilePicker sidebar common
  locations),
- `LibGUI/FileIconProvider` / `FileSystemModel` (the Desktop icon),
- `LibWebView` and the Browser (download directory),
- `SpiceAgent` (file transfers).

## Updater and query tool

`scripts/sde-user-dirs.sh` is the spec's updater and a small `xdg-user-dir`
equivalent:

```sh
sde-user-dirs.sh --update          # create user-dirs.dirs + the directories
sde-user-dirs.sh --query DOWNLOAD  # like xdg-user-dir DOWNLOAD
sde-user-dirs.sh PICTURES          # same as --query
```

`--update` writes the eight standard keys with `$HOME/...` values **only when
the file does not exist**, then creates each configured directory (`mkdir -p`).
Existing custom values are preserved. The session calls it once at login
(`sde-session.sh`, before autostart/apps).

`install-sde.sh` installs the script with the other runtime scripts. A host
`xdg-user-dir`/`xdg-user-dirs-update` stays compatible: it reads and writes the
same file.

## Tests

`Tests/UserDirsTest.cpp` (driven by `scripts/run-user-dirs-test.sh`) sets a
private `HOME`/`XDG_CONFIG_HOME`, writes a `user-dirs.dirs` with quoted values,
`${HOME}`, and a disabled (`$HOME`) entry, and asserts:

- `desktop`/`downloads` follow the file,
- `music` disabled resolves to `$HOME`,
- unset keys fall back to `$HOME/<Name>` (including Templates/Public),
- the environment override still wins.

## Not implemented

- **Localized default names** (`user-dirs.locale` and the `xdg-user-dirs-update`
  translations). The updater uses the English names, which the spec treats as
  the C-locale defaults; a user can edit `user-dirs.dirs` freely.
- **A GUI editor** for the directories. Editing the file (or running the
  updater/tool) is the supported path for now.
