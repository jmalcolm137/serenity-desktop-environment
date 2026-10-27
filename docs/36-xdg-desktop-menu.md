# XDG desktop menu

**Status:** implemented · **Related:** [`14-r4.4-config.md`](14-r4.4-config.md),
[`35-xdg-user-dirs.md`](35-xdg-user-dirs.md)

The Taskbar's system menu can be built either from the SerenityOS `.af`
application files (the default, "Serenity style") or from the freedesktop
[Desktop Menu Specification](https://specifications.freedesktop.org/menu-spec/latest/)
("XDG style"). The choice is a compositor config setting:

```ini
# $XDG_CONFIG_HOME/sde/WindowServer.ini
[Taskbar]
XDGMenu=false   ; false (default) = Serenity .af menu, true = XDG menu
```

`XDGMenu` is part of the shared schema: the compositor validates it (rejecting
anything that is not a boolean — `true`/`false`, `yes`/`no`, `on`/`off`, `1`/`0`)
and prints it under `[Taskbar]` in `scripts/sde-config list`. The Taskbar reads
the same layered file (user file over `$SERENITY_RES_ROOT/../etc/WindowServer.ini`)
so `sde-config` and the Taskbar cannot disagree. The change takes effect when the
Taskbar starts, like the other Taskbar settings.

```sh
scripts/sde-config set Taskbar XDGMenu true
```

## Serenity style (default)

`main.cpp:build_system_menu` discovers the installed `.af` files through
`Desktop::AppFile::for_each`, groups them by their `Category` (including
`Games/Puzzles`-style subpaths) and shows the `SystemMenu.ini` category icons
with each app's own icon. This is the unmodified SerenityOS behaviour, and it is
still the default: existing sessions and tests see no change.

## XDG style: the real menu specification

With `XDGMenu=true`, `XdgMenuFile.cpp` loads the session's menu file and applies
the specification; `XdgMenu.cpp` turns the resolved tree into GUI menus.

### Menu file locations

The menu file is found in this order:

1. `$XDG_CONFIG_HOME/menus/${XDG_MENU_PREFIX}applications.menu`
2. each `$XDG_CONFIG_DIRS/menus/${XDG_MENU_PREFIX}applications.menu`
3. if `$XDG_MENU_PREFIX` is set and none exists, the unprefixed
   `applications.menu` in the same directories;
4. the SDE's own `applications.menu` in `$SDE_MENU_DIR` (installed:
   `<prefix>/share/sde/menus`; development: `Base/xdg/menus`).

The first existing file wins (the spec's rule). The SDE session leaves
`XDG_MENU_PREFIX` unset, so the unprefixed menu (or SDE's default) is used;
exporting it in the environment (e.g. `plasma-`, as a KDE session does) selects a
host menu instead. A host with no menu file gets SDE's default menu rather than an
empty Taskbar menu.
`SDE_MENU_DIR` is exported by the session and the installed launcher; it also
defaults to `$SERENITY_RES_ROOT/../sde/menus` so a directly started Taskbar finds
an installed menu.

### Menu files

Every standard element used by real menus is handled:

- **Hierarchy:** `<Menu>`, `<Name>`, `<Directory>` and nested menus.
- **Defaults and extra directories:** `<DefaultAppDirs>`/`<AppDir>`,
  `<DefaultDirectoryDirs>`/`<DirectoryDir>`. Relative paths resolve against the
  menu file, and the default search paths are expanded in spec priority order
  (the earlier XDG data dir wins; `$XDG_DATA_HOME` is included).
- **Entry selection:** `<Include>` and `<Exclude>` in document order, with
  `<All>`, `<And>`, `<Or>`, `<Not>`, `<Category>` and `<Filename>` rules.
  `<Filename>` matches the desktop-file id (including the `.desktop` suffix;
  subdirectories are flattened with `-`).
- **Allocation:** `<OnlyUnallocated>`/`<NotOnlyUnallocated>`, processed in the
  spec's two passes. An entry included by a normal menu is marked allocated even
  if a later `<Exclude>` removes it, so it does not reappear in an `OnlyUnallocated`
  menu.
- **Merging:** `<MergeFile>` (both `type="path"` and `type="parent"`),
  `<MergeDir>`, `<DefaultMergeDirs>` (the default `applications-merged`
  directories; the per-user one is merged last for convenience). Duplicate child
  menus with the same `<Name>` are consolidated with the last menu's properties
  winning, and the earlier menus' entries/queries are prepended so later
  overrides still apply.
- **Editing:** `<Deleted>`/`<NotDeleted>`, and `<Move>` with `<Old>`/`<New>` paths
  (single- and multi-component paths; a move onto an existing menu merges them).
- **Layout:** `<Layout>` and inherited `<DefaultLayout>` with `<Menuname>`,
  `<Filename>`, `<Separator>` and `<Merge type="menus"|"files"|"all">`, including
  the `show_empty` attribute. Menus without a layout use the inherited default,
  then the specification's `<Merge type="menus"/><Merge type="files"/>` default.
  Unmentioned entries/submenus are omitted when an explicit layout has no `<Merge>`
  node, exactly as the spec defines. `<Menuname>` references a submenu by its
  `<Name>`, not by its visible label.
- **Directory entries:** `.directory` files from the menu's directory dirs give
  submenus their visible `Name` (localized), `Icon` and `Comment`. The last
  existing `<Directory>` wins.

### Desktop entries

Entries are collected from the menu's app dirs (own plus ancestors, child dirs
taking priority over parent dirs, later `<AppDir>` elements over earlier ones).
A `.desktop` file is shown if:

- `Type=Application` or `Type=Link`;
- `Hidden`/`NoDisplay` are not true;
- `OnlyShowIn`/`NotShowIn` match the session's `XDG_CURRENT_DESKTOP`
  (colon-separated; `SDE` and `Serenity` are the defaults), and
- `TryExec` (if present) resolves.

`Name` and `Comment` are localized (`Name[lang_COUNTRY]`, `Name[lang]`, honouring
`LANGUAGE`, `LC_ALL`, `LC_MESSAGES` and `LANG`). Launching follows the entry:
`Exec` is split with quote/backslash handling and field codes dropped; a missing
command is skipped; `Terminal=true` runs it through Serenity's Terminal; and
`Type=Link` entries open their `URL` through the Serenity launcher.

### Icons

An `Icon=` that is a path/URI is loaded directly. A themed icon name is tried as
a Serenity resource icon (`resource://icons/16x16/<name>.png`) and with the
common `applications-`/`preferences-`/`kf5-`/`gnome-` prefixes stripped.
Directory names that match the shipped `SystemMenu.ini` labels get the same
category icon as the Serenity menu; otherwise the entry's executable-derived icon
is used.

### The SDE's default menu

`Base/xdg/menus/applications.menu` is a spec-compliant default used when the host
has no menu file. It defines Development, Education, Games, Graphics, Internet,
Multimedia, Office, Settings, System, Utilities and an `OnlyUnallocated` Other
menu, with matching `.directory` files under
`Base/xdg/desktop-directories/`. `install-sde.sh` copies both to
`<prefix>/share/sde/{menus,desktop-directories}`.

### Fallback

If no menu file exists anywhere, the Taskbar logs
`no XDG menu file found; using the category fallback` and builds the old flat
category menu from the `.desktop` entries, so `XDGMenu=true` never leaves the
system menu empty.

## The Serenity applications' entries

Serenity apps ship `.af` metadata, not `.desktop` files. The session generates
freedesktop entries from them at login:

```sh
scripts/generate-desktop-entries.sh --output "$XDG_CONFIG_HOME/sde/xdg-data"
```

The generator reads `Base/res/apps/*.af`, maps the Serenity category to a
freedesktop main category, strips `&` mnemonics from `Name`, and writes one entry
per app (skipping `ExcludeFromSystemMenu=true`) with `Type=Application`,
`Exec=/bin/<App>` (resolved by the port to the session app directory),
`TryExec=/bin/<App>`, the `.af` `Arguments`, and `OnlyShowIn=SDE;` +
`X-SerenityOS=true`. `sde-session.sh` regenerates the tree into
`$XDG_CONFIG_HOME/sde/xdg-data` before the compositor starts and prepends it to
`XDG_DATA_DIRS` (`SDE_XDG_DATA_UPDATE=0` disables the regeneration, for tests).
`install-sde.sh` also generates entries into `<prefix>/share/applications` and
exports `XDG_DATA_DIRS=<prefix>/share:…` from the `sde-session` launcher, so an
installed session works before the per-user tree exists.

The generated entries are deliberately machine-written and start with
`# Generated by sde-generate-desktop-entries; do not edit.`; a user can drop
hand-written entries into `$XDG_DATA_HOME/applications` (higher priority) to
override or extend them.

## Tests

`scripts/run-taskbar-xdg-menu-test.sh` starts the Taskbar headless in four cases:

1. a user `applications.menu` that `<MergeFile type="parent">`-merges a system
   menu: exercises merge, duplicate-menu consolidation, `Directory` labels,
   `Include`/`Exclude`, `OnlyUnallocated`, `Deleted`, `Layout`/`DefaultLayout`,
   `Menuname`/`Filename`/`Separator`/`Merge`, localized names, `Link` entries,
   `OnlyShowIn`, `NoDisplay` and `TryExec` filtering;
2. the shipped `Base/xdg/menus/applications.menu` with no host menu;
3. no menu file anywhere, checking the category fallback;
4. `XDGMenu=false`, checking the Serenity `.af` menu.

Separately, the compositor's `sde-config` schema is covered by
`scripts/run-config-test.sh` (default `XDGMenu=false`, `set`/`get`, and rejection
of an invalid value) and by the config fuzz test. `run-install-layout-test.sh`
checks that the menu and `.directory` files are installed and that the launcher
exports `SDE_MENU_DIR`.

All runs use a private `XDG_RUNTIME_DIR`, `SDE_STATE_DIR`,
`XDG_CONFIG_HOME`/`XDG_CONFIG_DIRS` and data dirs, so they never touch a live
session or the developer's menus.

## Known gaps

- `<LegacyDir>` and `<KDELegacyDirs>` are ignored: the pre-XDG menu hierarchies
  they point at are not installed by supported hosts. `<Move>` is implemented.
- `inline` layout hints (`inline`, `inline_limit`, `inline_header`,
  `inline_alias`) are ignored; menus are always nested. This is allowed by the
  spec (layout hints are optional).
- The `.menu` DTD is not validated; the XML is parsed as well-formed XML and
  unknown elements/attributes are ignored (as the spec requires).
- XDG Icon Theme is still not implemented: themed names resolve against
  SerenityOS's resource icons, then fall back to executable-derived icons.
- `Actions`, `MimeType` handlers, `Keywords`, `StartupNotify`,
  `DBusActivatable`, and full menu editing are outside this menu implementation.
- The menu is read once at Taskbar start; a newly installed menu or entry needs a
  Taskbar restart (or `sde-session` restart), unlike the compositor config which
  live-reloads.
