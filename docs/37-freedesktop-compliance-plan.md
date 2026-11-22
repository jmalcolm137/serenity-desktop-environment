# Freedesktop.org Compliance Plan

**Status:** In progress (Phases F1–F4 landed) · **Date:** 2026-11-22
**Related:** [`03-engineering-conventions.md`](03-engineering-conventions.md) ·
[`06-service-layering.md`](06-service-layering.md) ·
[`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md) ·
[`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md) ·
[`24-libgui-wayland-sde-boundary.md`](24-libgui-wayland-sde-boundary.md) ·
[`25-performance.md`](25-performance.md) ·
[`35-xdg-user-dirs.md`](35-xdg-user-dirs.md) ·
[`36-xdg-desktop-menu.md`](36-xdg-desktop-menu.md)

## 1. Goal & Core Constraint

SerenityOS was designed as a self-contained operating system with its own
conventions (`.af` app files, `LaunchServer.ini`, `/res/icons`, `/tmp/session`).
SDE's mission is to bring that authentic SerenityOS UX to Linux **while keeping
imported SerenityOS sources as unmodified as possible**.

At the same time, a proper Linux desktop environment must interoperate smoothly
with host Linux applications (GTK, Qt, Electron, Flatpak, CLI tools) by
implementing the **freedesktop.org (XDG)** specifications.

### Architectural rule: implement XDG *around* SerenityOS, not *inside* it

Every remaining freedesktop.org specification can be implemented **with zero (or
near-zero) new patches to upstream SerenityOS** by placing the logic in the
layers SDE already owns:

| Layer (SDE-owned) | Freedesktop responsibilities |
|---|---|
| **`Libraries/LibXDG/`** *(new)* | Shared, zero-GLib AK/LibCore parsers & resolvers (`BaseDirs`, `DesktopEntry`, `MimeDatabase`, `MimeApps`, `IconTheme`, `Trash`, `RecentlyUsed`, `SoundTheme`). |
| **`Services/LaunchServer/`** | MIME type detection (`shared-mime-info`), default-app lookup (`mimeapps.list`), `.desktop` execution (`%f`/`%u`, `Path=`, `Terminal=`, `DBusActivatable=`, `xdg-activation-v1`), `recently-used.xbel` recording. |
| **`Services/WindowServer/`** | `XDG_STATE_HOME` / `XDG_CONFIG_HOME` separation, `XDG::IconTheme` fallback icons by `app_id`/`WM_CLASS`, `xdg-activation-v1` token handoff. |
| **`Tools/portal/`** | `xdg-desktop-portal-serenity` (`OpenURI`, `AppChooser`, `Trash`, `Settings` `reduced-motion`, `Notification` actions) sharing `LibXDG` with `LaunchServer`. |
| **`Tools/sde-notifications/`** | Full `org.freedesktop.Notifications` v1.2 (markup stripping, actions, themed icons via `XDG::IconTheme`). |
| **`Applets/Host/` (`sde-applet-host`)** | Hosts a new `StatusNotifier` tray applet (`org.kde.StatusNotifierWatcher` + `com.canonical.dbusmenu`) inside the existing multi-applet process—zero new resident processes. |
| **`scripts/` & `Base/`** | Full XDG Autostart (`$XDG_CONFIG_DIRS/autostart` + user overrides), `systemd --user` `graphical-session.target`, `Base/xdg/sde-mimeapps.list`, keyring session integration. |

All new C++ code follows [`03-engineering-conventions.md`](03-engineering-conventions.md):
AK/LibCore/LibMain, `dbus-1` behind `Tools/portal/dbus.h` (never GLib/GIO), and
strict memory discipline ([`25-performance.md`](25-performance.md)).

---

## 2. Current Freedesktop.org Audit Matrix

| # | Specification | Status | Current SDE implementation | Remaining gaps | Upstream SerenityOS patch impact |
|---|---|---|---|---|---|
| 1 | **XDG Desktop Portal** (`impl.portal.*`) | **Done** | `Tools/portal/` + `Dialogs/*` + `Base/xdg-desktop-portal/` (28 interfaces, [`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md)) | Add `Trash` portal; expose `reduced-motion` in `Settings`; share `LibXDG` in `OpenURI`/`AppChooser` | None |
| 2 | **Desktop Notifications** (`org.freedesktop.Notifications` v1.2) | **Done** | `Tools/sde-notifications/` + `Base/dbus-1/services/` | `icon` lookup via `XDG::IconTheme`; `body-markup` sanitization; action buttons | None |
| 3 | **XDG User Directories** (`user-dirs.dirs`) | **Done** | `libgui-wayland` patch `0072` + `scripts/sde-user-dirs.sh` ([`35-xdg-user-dirs.md`](35-xdg-user-dirs.md)) | Optional `user-dirs.locale` localized defaults | None |
| 4 | **Cursor Theme** (`XCURSOR_THEME` / `XCURSOR_SIZE`) | **Done** | `Services/WindowServer/src/chrome/Appearance.cpp` + `patches/0017` | None | None |
| 5 | **Wayland Activation** (`xdg-activation-v1`) | **Done** | `Services/WindowServer/src/Interop.cpp` | Pass activation tokens from `LaunchServer` / Taskbar when launching `StartupNotify=true` apps | None |
| 6 | **Desktop Menu** (`applications.menu`, `.directory`) | **Partial (opt-in)** | `patches/0009` + `Base/xdg/menus/` + `Base/xdg/desktop-directories/` ([`36-xdg-desktop-menu.md`](36-xdg-desktop-menu.md)) | Fix `XDG_DATA_DIRS` clobbering `/usr/local/share:/usr/share` in `sde-session.sh`; evaluate defaulting `[Taskbar] XDGMenu=true`; `XDG::IconTheme` icons; live reload via `inotify` | None (extends existing SDE-only `patches/0009`) |
| 7 | **Desktop Entry** (`.desktop` v1.5) | **Partial** | `scripts/generate-desktop-entries.sh`, `XdgMenu.cpp`, `sde-session.sh` | Cross-desktop Serenity entries (drop blanket `OnlyShowIn=SDE;`, host `Exec`/`TryExec` via `sde-app` wrapper, install into `/usr/share/applications/serenity-*.desktop` + `/usr/share/icons/hicolor/`); field-code expansion (`%f`/`%F`/`%u`/`%U`/`%i`/`%c`/`%k`), `Path=`, `DBusActivatable=`, `Desktop Actions` | None |
| 8 | **Desktop Application Autostart** | **Partial** | `scripts/sde-session.sh` scans `$XDG_CONFIG_HOME/autostart/*.desktop` | `$XDG_CONFIG_DIRS/autostart` precedence/override, `OnlyShowIn`/`NotShowIn`, `Hidden=true`, `TryExec=` | None |
| 9 | **XDG Base Directory** | **Partial** | `XDG_CONFIG_HOME`, `XDG_DATA_DIRS`, `XDG_RUNTIME_DIR` used | Separate `XDG_STATE_HOME` (`wallpaper.raw`, `sessions/`, `secrets/`, `xwayland-display`) and `XDG_CACHE_HOME` from `XDG_CONFIG_HOME`; `XDG_CONFIG_DIRS` layering | None |
| 10 | **MIME Applications Associations** (`mimeapps.list`) | **Missing** | `LaunchServer` uses Serenity `LaunchServer.ini` only | Full spec lookup order + `[Default Applications]` / `[Added/Removed Associations]` + `Base/xdg/sde-mimeapps.list` | None |
| 11 | **Shared MIME-info Database** (`shared-mime-info`) | **Missing** | Serenity extension-based detection only | Parse `/usr/share/mime/{globs2,aliases,subclasses,magic}` + `$XDG_DATA_HOME/mime` in `LibXDG` | None |
| 12 | **Icon Theme Specification** (`index.theme`, `hicolor`) | **Missing** | `XdgMenu.cpp` only checks Serenity `/res/icons/16x16/` | Full `XDG::IconTheme` lookup (`$XDG_DATA_HOME/icons`, `$XDG_DATA_DIRS/icons`, `hicolor`, `/usr/share/pixmaps`, PNG+SVG via `LibGfx`) | None |
| 13 | **StatusNotifierItem / AppIndicator Tray** | **Missing** | Native `GUI::WindowType::Applet` area only | SDE tray applet (`org.kde.StatusNotifierWatcher` + `com.canonical.dbusmenu`) hosted in `sde-applet-host` | None |
| 14 | **Trash Specification** (`$XDG_DATA_HOME/Trash`) | **Missing** | Serenity direct delete / non-XDG trash | `XDG::Trash` in `LibXDG`, `Tools/sde-trash`, `Trash` portal; optional minimal hook in `FileManager` | Optional (~20-line `FileManager` hook) |
| 15 | **Thumbnail Managing Standard** (`$XDG_CACHE_HOME/thumbnails`) | **Missing** | Per-session `ImageDecoder` decodes only | Read/write freedesktop `normal` PNG thumbnails (`Thumb::URI`, `Thumb::MTime`) in `Services/ImageDecoder` | None |
| 16 | **Secret Service API** (`org.freedesktop.secrets`) | **Missing** | Portal `Secret` uses local file; `WifiApplet` has NM `SecretAgent` | Session/PAM integration with standard providers (`gnome-keyring-daemon` / `oo7-daemon` / `keepassxc`) | None |
| 17 | **systemd User Session** (`graphical-session.target`) | **Missing** | `dbus-update-activation-environment` only | `Base/systemd/user/sde-session.target` + `systemctl --user` start/stop in `sde-session.sh` | None |
| 18 | **Recently Used Files** (`recently-used.xbel`) & **Sound Theme** | **Missing** | None | `XDG::RecentlyUsed` (`LaunchServer`, `Dialogs/FileChooser`) and `XDG::SoundTheme` (`AudioServer`) | None |

---

## 3. Core Enabler: `Libraries/LibXDG/`

Rather than scattering ad-hoc `.desktop`, MIME, or icon parsers across
`LaunchServer`, `WindowServer`, `Tools/portal/`, `Tools/sde-notifications/`,
and `Taskbar`, we introduce one small static library **`Libraries/LibXDG/`**
(`libxdg.a`) built purely on `AK` + `LibCore` (plus optional `LibGfx` helper for
icon loading).

### Proposed module layout

```text
Libraries/LibXDG/
  CMakeLists.txt
  BaseDirs.h / .cpp        # XDG_CONFIG_HOME/DIRS, DATA_HOME/DIRS, STATE_HOME, CACHE_HOME, RUNTIME_DIR
  DesktopEntry.h / .cpp    # .desktop parser (localization, OnlyShowIn/NotShowIn, TryExec, Exec field codes, Actions, DBusActivatable)
  MimeDatabase.h / .cpp    # shared-mime-info (globs2, aliases, subclasses, magic sniffing)
  MimeApps.h / .cpp        # mimeapps.list precedence resolution & default-app updates
  IconTheme.h / .cpp       # index.theme inheritance, size matching, hicolor + pixmaps fallback
  Trash.h / .cpp           # $XDG_DATA_HOME/Trash & per-mount .Trash-$uid (.trashinfo atomic writes)
  RecentlyUsed.h / .cpp    # $XDG_DATA_HOME/recently-used.xbel reader/writer
  SoundTheme.h / .cpp      # $XDG_DATA_DIRS/sounds index.theme lookup
```

### Memory & performance rules for `LibXDG`
- **Lazy & bounded:** parse `globs2` / `index.theme` once on first query and
  invalidate on directory `mtime` change—never scan `/usr/share/icons`
  recursively at startup.
- **Zero new daemons:** `LibXDG` is a static library linked into existing
  processes (`LaunchServer`, `WindowServer`, `xdg-desktop-portal-serenity`,
  `sde-applet-host`).
- **Pure-logic unit tests:** every module is testable in milliseconds under
  `Tests/` with temporary `XDG_*` directories and no compositor.

---

## 4. Phased Implementation Plan

### Phase F1 — Session & Base Directory Foundations *(Implemented)*
**Goal:** Make the session lifecycle, config/state layout, and autostart strictly
spec-compliant before layering higher-level XDG features.
**SerenityOS upstream patches:** 0.

1. **`Libraries/LibXDG/BaseDirs` & Base Directory hygiene:**
   - Separate config (`$XDG_CONFIG_HOME/sde`: `WindowServer.ini`, `session.conf`,
     `input.ini`, `cursor.ini`, `screenlayout.ini`, `global-shortcuts.ini`) from
     ephemeral/mutable state (`$XDG_STATE_HOME/sde`: `session.log`,
     `wallpaper.raw`, `sessions/`, `secrets/`, `xwayland-display`,
     `xdg-data/`) and cache (`$XDG_CACHE_HOME/sde`).
   - Keep `SDE_STATE_DIR` override and automatic read-fallback to the old
     `$XDG_CONFIG_HOME/sde/` location so existing users upgrade seamlessly.
   - Layer `$XDG_CONFIG_DIRS/sde/WindowServer.ini` between user config and
     built-in defaults.
2. **Full XDG Autostart (`Libraries/LibXDG/DesktopEntry` + `Tools/sde-autostart` or `scripts/sde-session.sh`):**
   - Scan `$XDG_CONFIG_HOME/autostart` and every `$XDG_CONFIG_DIRS/autostart`
     directory with exact filename-override precedence (a user file with the
     same basename overrides or disables a system file via `Hidden=true`).
   - Enforce `OnlyShowIn=` / `NotShowIn=` against `XDG_CURRENT_DESKTOP` (`SDE`),
     `TryExec=`, `Path=`, `Terminal=true`, and `X-GNOME-Autostart-enabled=false`.
3. **systemd user session integration:**
   - Add `Base/systemd/user/sde-session.target` (`BindsTo=graphical-session.target`,
     `Wants=graphical-session-pre.target`).
   - In `scripts/sde-session.sh`, import session env into `systemd --user`,
     start `sde-session.target` once `WindowServer` is ready, and stop it cleanly
     on logout (no-op when systemd is not PID 1 / not running a user manager).

**Verification:**
- `Tests/XdgBaseDirsTest.cpp` & `Tests/XdgAutostartTest.cpp` (`scripts/run-autostart-test.sh`).
- Verify existing `scripts/run-session-test.sh`, `scripts/run-config-test.sh`,
  and `scripts/run-install-layout-test.sh` pass.

---

### Phase F2 — Cross-Desktop App Menu, `.desktop` Interop & MIME Associations (`shared-mime-info`, `mimeapps.list`) *(Implemented)*
**Goal:** (a) The SDE Taskbar menu shows the full set of installed Linux + Serenity
applications just like other desktops, (b) SerenityOS applications appear with
real icons and launch cleanly from other Linux desktops (KDE, GNOME, XFCE, …),
and (c) clicking any file or URL opens the user's configured default Linux or
Serenity application everywhere.
**SerenityOS upstream patches:** 0.

1. **Fix Taskbar menu visibility (`scripts/sde-session.sh` & `[Taskbar] XDGMenu`):**
   - Fix `XDG_DATA_DIRS` in `scripts/sde-session.sh` so prepending `$SDE_XDG_DATA`
     preserves `${XDG_DATA_DIRS:-/usr/local/share:/usr/share}` (plus Flatpak
     export dirs if present) instead of wiping out `/usr/share/applications`
     when `XDG_DATA_DIRS` is unset at login.
   - Make `[Taskbar] XDGMenu=true` the default for Linux sessions (keeping
     `XDGMenu=false` available for classic Serenity-only `.af` mode).
2. **Make SerenityOS apps first-class citizens in other Linux desktops (`generate-desktop-entries.sh`, `sde-app`, `install-sde.sh`):**
   - Add a tiny `sde-app` launcher wrapper (`$PREFIX/bin/sde-app <App> [args...]`)
     that sets `SERENITY_RES_ROOT`, `LD_LIBRARY_PATH`, and `LIBWM_APP_BIN_DIR`
     when launched outside an SDE session, then `exec`s the real Serenity binary.
   - Update `scripts/generate-desktop-entries.sh` to:
     - drop the blanket `OnlyShowIn=SDE;` (keeping `OnlyShowIn=SDE;` only for
       SDE-session-specific settings tools like `DisplaySettings`,
       `MouseSettings`, `KeyboardSettings`, `NetworkSettings`);
     - emit real host `TryExec=$PREFIX/bin/<App>` and `Exec=$PREFIX/bin/sde-app <App> %U`
       (instead of Serenity-only `/bin/<App>` which fails `TryExec` on Linux);
     - use `serenity-<id>.desktop` filenames to avoid colliding with host
       `Terminal.desktop` / `Calculator.desktop` / `Browser.desktop`;
     - emit host-resolvable `Icon=` names/paths and include `MimeType=` lines
       from `.af` metadata.
   - Update `scripts/install-sde.sh` to install the generated
     `serenity-*.desktop` files into `$DESTDIR/usr/share/applications/` and the
     Serenity app PNG icons into `$DESTDIR/usr/share/icons/hicolor/{16x16,32x32}/apps/`
     so KDE, GNOME, XFCE, and `xdg-open` discover them with real icons.
3. **`Libraries/LibXDG/MimeDatabase` (`shared-mime-info`):**
   - Parse `globs2` (weighted glob matching, case sensitivity), `aliases`,
     `subclasses` (e.g. `text/x-c++src` → `text/plain`), and `magic` (header
     sniffing when globs are absent or ambiguous) across `$XDG_DATA_HOME/mime`
     and `$XDG_DATA_DIRS/mime`.
4. **`Libraries/LibXDG/MimeApps` (`mimeapps.list`):**
   - Implement exact spec search order:
     1. `$XDG_CONFIG_HOME/$desktop-mimeapps.list`
     2. `$XDG_CONFIG_HOME/mimeapps.list`
     3. `$XDG_CONFIG_DIRS/$desktop-mimeapps.list` → `$XDG_CONFIG_DIRS/mimeapps.list`
     4. `$XDG_DATA_HOME/applications/$desktop-mimeapps.list` → `$XDG_DATA_HOME/applications/mimeapps.list`
     5. `$XDG_DATA_DIRS/applications/$desktop-mimeapps.list` → `$XDG_DATA_DIRS/applications/mimeapps.list`
   - Honour `[Default Applications]`, `[Added Associations]`,
     `[Removed Associations]`, and MIME subclass inheritance.
   - Ship `Base/xdg/sde-mimeapps.list` mapping common types to Serenity apps
     when installed, with standard Linux fallbacks.
5. **Full `.desktop` execution in `Libraries/LibXDG/DesktopEntry`:**
   - Implement `Exec=` quoting and field codes (`%f`, `%F`, `%u`, `%U`, `%i`,
     `%c`, `%k`), `Path=`, `Terminal=true` (spawning Serenity `Terminal`),
     `DBusActivatable=true` (calling `org.freedesktop.Application.Activate` /
     `Open` over `Tools/portal/dbus.h`), and `Desktop Actions`.
   - When `StartupNotify=true`, request an `xdg-activation-v1` token and export
     `XDG_ACTIVATION_TOKEN` / `DESKTOP_STARTUP_ID` to the launched child.
6. **Wire into SDE services:**
   - **`Services/LaunchServer/`:** resolve file/URL handlers via `LibXDG`
     (`MimeDatabase` + `MimeApps` + `DesktopEntry`) first, falling back to
     Serenity's `LaunchServer.ini` for Serenity-specific handlers.
   - **`Tools/portal/backend.cpp` (`OpenURI`, `AppChooser`) & `Dialogs/AppChooser/`:**
     use `LibXDG` for candidate app discovery, default selection, and
     persisting "Always open with" choices to `$XDG_CONFIG_HOME/mimeapps.list`.

**Verification:**
- `Tests/XdgMimeTest.cpp` (globs, subclasses, aliases, `mimeapps.list` precedence,
  `Removed Associations`, `Exec=` field-code expansion).
- `scripts/run-mimeapps-test.sh` (end-to-end `LaunchServer` + `OpenURI` portal
  with custom `mimeapps.list` and fake `.desktop` handlers).
- `scripts/run-taskbar-xdg-menu-test.sh` & `scripts/run-install-layout-test.sh`
  extended to verify `XDG_DATA_DIRS` preserves `/usr/share/applications` and
  generated `serenity-*.desktop` entries pass `TryExec` / `OnlyShowIn` / icon
  checks for non-SDE desktops.

---

### Phase F3 — Icon Theme Specification & System Tray (`StatusNotifierItem`) *(Implemented)*
**Goal:** Host Linux applications have crisp icons everywhere (menu, titlebars,
Alt+Tab, notifications, dialogs) and third-party tray icons appear natively in
the Serenity Taskbar applet area.
**SerenityOS upstream patches:** 0 new patches (only updates existing SDE-only
`patches/0009` for Taskbar's XDG menu).

1. **`Libraries/LibXDG/IconTheme`:**
   - Parse `index.theme` (`Inherits=`, `Directories=`, `Size=`, `Scale=`,
     `MinSize=`, `MaxSize=`, `Type=Fixed|Scalable|Threshold`).
   - Search `$XDG_DATA_HOME/icons` → `$HOME/.icons` → `$XDG_DATA_DIRS/icons`
     (including Flatpak exports) → `/usr/share/pixmaps`, always falling back to
     `hicolor`.
   - Decode PNG (and SVG via `LibGfx::SVGImageDecoderPlugin` when available)
     scaled cleanly to the requested pixel size (`16x16` for Taskbar/titlebars,
     `32x32` for notifications/dialogs).
   - Add `[Theme] IconTheme=` to `WindowServer.ini` (default `hicolor`,
     configurable via `sde-config`).
2. **Wire `XDG::IconTheme` into SDE components:**
   - **`Services/WindowServer/`:** when a Wayland (`xdg_toplevel`) or X11 window
     does not provide its own icon (`set_icon` / `_NET_WM_ICON`), look up its
     `app_id` / `WM_CLASS` via `DesktopEntry` + `IconTheme` so host apps get
     real titlebar, Alt+Tab, and Taskbar icons automatically.
   - **`Tools/sde-notifications/`:** resolve `app_icon` theme names via
     `XDG::IconTheme`.
   - **`Dialogs/AppChooser/`:** display real app icons alongside names.
   - **Taskbar XDG menu (`patches/0009`):** resolve `.desktop` and `.directory`
     `Icon=` entries through `XDG::IconTheme` (falling back to Serenity
     `/res/icons/16x16/`), and add `Core::FileWatcher` live reload on
     `applications/` dirs so newly installed packages appear without restarting
     the Taskbar.
3. **StatusNotifierItem / AppIndicator tray applet (`Applets/StatusNotifier/`):**
   - Implement `org.kde.StatusNotifierWatcher` (and `org.freedesktop.StatusNotifierWatcher`
     alias) on the session D-Bus using `Tools/portal/dbus.h` (zero GLib).
   - Host it inside **`sde-applet-host`** (`Applets/Host/main.cpp`) so it adds
     **zero new processes** to the session!
   - For each registered `StatusNotifierItem`, create a `GUI::WindowType::Applet`
     window in the Taskbar applet area:
     - Render `IconName` (via `XDG::IconTheme`) or raw ARGB `IconPixmap`
       (respecting `AttentionIconName` / `Status=Passive|Active|NeedsAttention`).
     - Show `ToolTip` on hover.
     - Left-click calls `Activate(x, y)`; middle-click calls `SecondaryActivate(x, y)`;
       scroll calls `Scroll(delta, orientation)`; right-click fetches
       `com.canonical.dbusmenu` (`GetLayout`) and renders a native Serenity
       `GUI::Menu` (sending `Event(id, "clicked", ...)` on activation).

**Verification:**
- `Tests/XdgIconThemeTest.cpp` (`index.theme` inheritance, size distance
  scoring, `hicolor` fallback, `pixmaps` fallback).
- `scripts/run-sni-tray-test.sh` (mock D-Bus `StatusNotifierItem` + `dbusmenu`
  client verifying applet window creation, icon rendering, click activation,
  and menu item dispatch).

---

### Phase F4 — Desktop Interop: Trash, Thumbnails, Notifications v1.2 & Keyring *(Implemented)*
**Goal:** Seamless file/data interoperability with host Linux apps and file
managers.
**SerenityOS upstream patches:** 0 required (1 optional ~20-line hook in
`FileManager` if we want `FileManager`'s Delete/Trash action to use XDG Trash).

1. **Trash Specification (`Libraries/LibXDG/Trash` + `Tools/sde-trash` + `Trash` portal):**
   - Implement atomic `.trashinfo` creation (`[Trash Info]`, URL-encoded `Path=`,
     `DeletionDate=YYYY-MM-DDTHH:MM:SS`) in `$XDG_DATA_HOME/Trash/{files,info}`
     and per-mount `.Trash-$uid` / `.Trash/$uid` when trashing across mount
     points.
   - Add `Tools/sde-trash` (`trash`, `list`, `restore`, `empty`).
   - Add `org.freedesktop.impl.portal.Trash` (`TrashFile`) to
     `xdg-desktop-portal-serenity` and `Base/xdg-desktop-portal/serenity.portal`.
   - *(Optional)* Small SDE patch in `FileManager` routing "Move to Trash"
     through `XDG::Trash`.
2. **Thumbnail Managing Standard (`$XDG_CACHE_HOME/thumbnails/normal`):**
   - In `Services/ImageDecoder/`, add an opt-in/default disk cache for file
     thumbnails: compute `MD5(canonical_file_uri)`, validate `Thumb::URI` and
     `Thumb::MTime` PNG `tEXt` chunks, serve cache hits immediately, and write
     `128x128` (`normal`) PNGs atomically (`rename`) on cache miss.
   - Speeds up `FileManager` image browsing and shares thumbnails with
     Dolphin/Nautilus/GTK/Qt file choosers.
3. **Notification & Portal polish:**
   - `Tools/sde-notifications/`: strip/normalize basic HTML markup
     (`<b>`, `<i>`, `<u>`, `<a href>`) so `body-markup` apps don't display raw
     tags; add `actions` support where `NotificationServer` allows (or click
     forwarding for the `"default"` action).
   - `Tools/portal/backend.cpp`: expose `org.freedesktop.appearance`
     `reduced-motion` derived from `[Effects]` in `WindowServer.ini`.
4. **Secret Service API (`org.freedesktop.secrets`):**
   - Document and wire standard Linux keyring activation in `scripts/sde-session.sh`
     and `docs/13-native-session-and-aur-packaging.md` (`gnome-keyring-daemon`,
     `oo7-daemon`, or `keepassxc` via PAM / D-Bus activation)—do **not** build a
     bespoke crypto keyring daemon in SDE.

**Verification:**
- `Tests/XdgTrashTest.cpp` (trash/restore/collision numbering/URI encoding).
- `scripts/run-portal-test.sh` extended for `org.freedesktop.impl.portal.Trash`
  and `Settings` `reduced-motion`.
- `scripts/run-filemodel-thumbnail-test.sh` extended to verify XDG thumbnail
  cache hit/invalidation.

---

### Phase F5 — Polish: Recently Used Files, Sound Themes & Localized User Dirs
**Goal:** Complete the long tail of desktop standards.
**SerenityOS upstream patches:** 0.

1. **`Libraries/LibXDG/RecentlyUsed` (`$XDG_DATA_HOME/recently-used.xbel`):**
   - Record opened files from `Services/LaunchServer/` and `Dialogs/FileChooser/`
     (bounded to e.g. 200 entries, atomic write).
   - Optionally surface a "Recent" shortcut in `Dialogs/FileChooser/`.
2. **`Libraries/LibXDG/SoundTheme` (`$XDG_DATA_DIRS/sounds`):**
   - Resolve freedesktop event sound names (`bell`, `message-new-instant`,
     `dialog-warning`, `device-added`) via `index.theme` + `freedesktop`
     fallback, played through `Services/AudioServer/`.
3. **Localized `user-dirs.dirs` (`user-dirs.locale`):**
   - In `scripts/sde-user-dirs.sh`, defer to host `xdg-user-dirs-update` when
     installed (preserving localized defaults automatically), falling back to
     the built-in C-locale updater when absent.

---

## 5. Guardrails & Non-Goals

- **No upstream SerenityOS bloat:** Zero Linux-only freedesktop code goes into
  upstream SerenityOS libraries (`LibGUI`, `LibGfx`, `LibCore`). Framework-level
  hooks (if ever needed) stay minimal in `libgui-wayland`; desktop-level specs
  live in `Libraries/LibXDG/` and SDE's own services/tools/applets.
- **No GLib/GIO runtime creep:** All SDE D-Bus services continue using AK/LibCore
  over `dbus-1` (`Tools/portal/dbus.h`).
- **Session memory budget ([`25-performance.md`](25-performance.md)):**
  Total idle session PSS must remain **< 50 MB**. New tray/portal/MIME features
  live in `Libraries/LibXDG` (static), `xdg-desktop-portal-serenity`, or
  `sde-applet-host`—never by spawning new LibGUI daemons.
- **SerenityOS UX preserved:** Native SerenityOS aesthetics (90s chrome, bitmap
  fonts, Serenity themes, Taskbar, classic `.af` menu option) remain first-class;
  freedesktop compliance makes host Linux apps feel native *inside* that UX
  rather than replacing it.

---

## 6. Recommended Execution Order

| Order | Phase | Status | User-visible impact | Effort | Upstream patches |
|---|---|---|---|---|---|
| 1 | **F1** (BaseDirs, full Autostart, `systemd --user`) | **Done** | Clean `~/.config` vs `~/.local/state`; system autostart apps & systemd user services work properly | Small | 0 |
| 2 | **F2** (Cross-desktop app menu & `.desktop` interop + `shared-mime-info` + `mimeapps.list` + full `.desktop` exec) | **Done** | Taskbar menu shows all Linux + Serenity apps; Serenity apps appear with real icons and launch cleanly in KDE/GNOME/XFCE; clicking any file/URL opens the right default app | Medium | 0 |
| 3 | **F3** (`XDG::IconTheme` + `StatusNotifierItem` tray + live XDG menu) | **Done** | Real icons for all Linux apps + Discord/Steam/Fcitx5/Nextcloud tray icons in Taskbar | Medium | 0 |
| 4 | **F4** (Trash, FileManager Trash hook, Notifications v1.2 markup, keyring wiring) | **Done** | Shared Trash with host Linux file managers + FileManager Delete -> XDG Trash; cleaner notifications & keyring unlock | Medium | 1 small hook (`patches/0030`) |
| 5 | **F5** (`recently-used.xbel`, Sound Theme, `user-dirs.locale`) | **Next** | Long-tail desktop polish | Small | 0 |
