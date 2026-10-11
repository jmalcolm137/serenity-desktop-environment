# xdg-desktop-portal-serenity (R5)

**Status:** Phase 1 + Phase 2 landed (see §5 for the remaining Phase 2b items) ·
**Related:** [`08-compositor-gaps.md`](08-compositor-gaps.md) §3,
[`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §R5,
`contrib/xdg-desktop-portal/`

## 1. What it is

A backend for the freedesktop **`xdg-desktop-portal`**: apps (sandboxed or not)
talk to the portal frontend, which routes to a backend implementing
`org.freedesktop.impl.portal.*`. KDE ships `xdg-desktop-portal-kde`, GNOME ships
`-gtk`; this is the SDE backend, **`xdg-desktop-portal-serenity`**.

It is a **portable host D-Bus service** (GIO/GDBus, standard protocols only — P8/P9):
it runs under any compositor, and reaches SDE-specific behaviour through small,
overridable seams. It aims for **standards compliance** (the documented impl
interfaces, no vendor-private surface).

- Bus name: `org.freedesktop.impl.portal.desktop.serenity`
- Object path: `/org/freedesktop/portal/desktop`
- `.portal` file: `contrib/xdg-desktop-portal/serenity.portal`
- Selection: `contrib/xdg-desktop-portal/portals.conf` (prefer us where we
  implement an interface, fall back to `gtk` otherwise)

This supersedes the earlier single-interface `tools/sde-settings-portal.c` (R4.4):
`Settings` now lives here alongside the other interfaces.

## 2. Interfaces

| Interface | Behaviour |
|---|---|
| `Settings` (v2) | `color-scheme` (from the active theme's `Window` luminance), `accent-color` (theme `Accent`/`Selection`), `contrast` — read from the theme broker (`$XDG_RUNTIME_DIR/sde-theme`); `SettingChanged` is emitted when the broker changes. |
| `Account` (v1) | `GetUserInformation` from the passwd entry (`name`, `id`; `image` empty). |
| `Lockdown` (v1) | Read-only properties; false by default, overridable by `SDE_PORTAL_LOCKDOWN_*`. |
| `Background` (v1) | `ApplicationName`/`ApplicationID`; `SetStatus`; `SetAutostart` writes an XDG autostart entry (the SDE supervisor honours XDG autostart); `SetWallpaperURI` writes the compositor wallpaper state. |
| `AppChooser` (v1) | `ChooseApplication` through the LibGUI **AppChooserDialog** app (`SDE_PORTAL_APP_CHOOSER`); falls back to the first offered application when no chooser is configured. |
| `Email` (v1) | `ComposeEmail` opens a `mailto:` URI. |
| `Location` (v1) | Reports "not available" (no location provider). |
| `ProxyResolver` (v1) | `Lookup` returns an empty list (direct connection). |
| `OpenURI` (v3) | `OpenURI`/`OpenDirectory`/`OpenFile` launch via `SDE_PORTAL_OPENER` (the session launcher) or GIO's default-app launch; `OpenFile` materialises its fd, `Activate` launches an app id. |
| `Notification` (v1) | `AddNotification`/`RemoveNotification` via `SDE_PORTAL_NOTIFIER` (the Serenity notify tool, called as `<id> <title> <body> <icon>`) or `notify-send`; exposes the `ActionInvoked`/`NotificationClosed` signals and passes a themed icon through. |
| `ScreenCast` (v6) | `CreateSession`/`SelectSources`/`Start`/`AvailableSourceTypes`/`AvailableCursorModes`. Captures an output **or a window** with the standard **`ext-image-copy-capture-v1`** and publishes a **PipeWire** video stream (`sde-screencast`); `Start` returns the stream node id + `pipewire-serial` (v6) + position/size/source_type/id. Cursor modes: hidden, embedded. Restore tokens (`persist_mode`) are persisted and returned from `SelectSources`/`Start`. |
| `Realtime` (v1) | Grants high-priority/realtime threads (accepted). |
| `PowerProfileMonitor` (v1) | `power-saver-enabled` (from `SDE_POWER_SAVER`). |
| `GameMode` (v1) | `RegisterGame`/`UnregisterGame`/`QueryStatus` (reports not active). |
| `MemoryMonitor` (v1) | `LowMemoryWarning` signal; `version` only. |
| `FileChooser` (v3) | `OpenFile`/`SaveFile`/`SaveFiles` (directory mode via the `directory` option) through the LibGUI **FileChooserDialog** app (`SDE_PORTAL_FILE_CHOOSER`); returns the chosen `uris`. |
| `Screenshot` (v2) | `Screenshot` captures the primary output (via the ScreenCast capture pipeline, on its own Wayland connection) into a PNG and returns its `uri`; with `interactive` the LibGUI **ScreenshotOverlay** app (`SDE_PORTAL_SCREENSHOT_OVERLAY`) lets the user drag an area, which is cropped out. `PickColor` freezes the frame and opens the LibGUI **ColorPicker** app (`SDE_PORTAL_COLOR_PICKER`); the clicked pixel is sampled and returned as a `(ddd)` color. |
| `Print` (v1) | `PreparePrint` via the LibGUI **PrintDialog** app (`SDE_PORTAL_PRINT_DIALOG`; printers from `lpstat`); `Print` feeds the document fd to `SDE_PORTAL_PRINT_CMD` (e.g. `lp`). |
| `RemoteDesktop` (v2) | `CreateSession`/`SelectDevices`/`Start` (devices + the paired ScreenCast stream) and the `Notify*` input methods — pointer motion/absolute/button/axis and keyboard keycodes — injected via `wlr-virtual-pointer` / `virtual-keyboard`. `ConnectToEIS` (libei) is not supported, so apps use the `Notify*` methods. Restore tokens are persisted and returned from `Start`. |
| `GlobalShortcuts` (v1) | `CreateSession`/`BindShortcuts`/`ListShortcuts`. Shortcuts are stored in the SDE **config system** (`global-shortcuts.ini`, `[ShortcutN] Session/Id/Trigger`) for the compositor to bind like any other keybinding; activations are reported over a session FIFO (`sde-global-shortcuts.fifo`) and emitted as the `Activated` signal. |
| `Wallpaper` (v1) | `SetWallpaper` decodes the image and writes the compositor's wallpaper state (`wallpaper.raw`/`.mode`), so a sandboxed app can set the desktop background. |
| `Secret` (v1) | `RetrieveSecret` returns a stable per-app 64-byte secret, generated once and kept under `secrets/` in the state directory. |
| `USB` (v1) | `ListDevices`/`AcquireDevices`/`ReleaseDevices` — no device broker yet, so an empty device list (standards-compliant). |
| `DynamicLauncher` (v1) | `RequestInstallToken` (random token), `PrepareInstall` (echoes the name), `Launch` (launches the desktop file). |
| `Access` (v1) | `AccessDialog` through the LibGUI **AccessDialog** app (`SDE_PORTAL_ACCESS_DIALOG`); returns allowed (0) / denied (1). |

### ScreenCast pipeline (niri-inspired)

The implementation follows **niri**'s producer design (`src/screencasting/`):

- The Wayland thread drives `ext-image-copy-capture-v1` on the selected output
  (`ext_output_image_capture_source_manager_v1` →
  `ext_image_copy_capture_session_v1` → per-frame capture into an SHM buffer).
- PipeWire runs on a `pw_thread_loop`; the stream is `Video/Source` with the
  `PW_STREAM_FLAG_DRIVER` and the same param set niri advertises — `EnumFormat`
  (video/raw/**BGRA** + size + framerate), `SPA_PARAM_Buffers` (memfd,
  `blocks=1`, `buffers` range 2..16) and `SPA_PARAM_Meta` (`SPA_META_Header`).
- A mutex-protected "latest frame" is memcpy'd into the negotiated buffer in the
  stream `process` callback, so captured frames reach consumers (~30 fps).

Still to add (niri parity, Phase 2b): **DMA-BUF** capture/negotiation (zero-copy)
and the **metadata** cursor mode (`ext_image_copy_capture_cursor_session_v1` +
`SPA_META_Cursor`). The interface advertises **v6** and returns the stream's
`pipewire-serial`; **restore tokens** are minted when a client sets
`persist_mode` and persisted under `screencast-tokens` in the state directory
(returned from `SelectSources`/`Start`, accepted back as `restore_token`). niri
extras (dynamic cast target, "block out from screencast") remain.

**Source selection is a LibGUI app.** As with all SDE UI, the picker is a
SerenityOS **LibGUI** app — `ScreenCastChooser/` (built by
`scripts/build-screen-cast-chooser.sh`) — launched by the backend when
`SDE_PORTAL_CHOOSER` is set, with the monitor + window candidates as arguments; it
prints the chosen index. Window sources use
`ext_foreign_toplevel_image_capture_source_manager_v1`, and the embedded cursor
mode uses the capture session's `PAINT_CURSORS` option.

The **FileChooser** portal is likewise a LibGUI app — `FileChooserDialog/`
(`scripts/build-file-chooser-dialog.sh`), wrapping `GUI::FilePicker` — launched
via `SDE_PORTAL_FILE_CHOOSER`. The **Print** dialog is `PrintDialog/`
(`scripts/build-print-dialog.sh`), and the permission prompt is `AccessDialog/`
(`scripts/build-access-dialog.sh`). The color picker is `ColorPicker/`
(`scripts/build-color-picker.sh`), and the application chooser is
`AppChooserDialog/` (`scripts/build-app-chooser-dialog.sh`). The interactive
screenshot area selector is `ScreenshotOverlay/`
(`scripts/build-screenshot-overlay.sh`). `scripts/build-portal-apps.sh` builds
all of them in one go.

> Note: programmatic widgets that draw text (such as `GUI::Label`) are only
> visible when the root widget paints the window background, so the dialogs call
> `set_fill_with_background_color(true)` on their main widget — exactly what GML
> dialogs declare (`fill_with_background_color: true`). Without it the client
> buffer stays black and black label text disappears.

## 3. SDE integration seams

Environment variables let the session point the backend at SDE components without
hard-coding them (the backend stays portable, with fallbacks):

- `SDE_PORTAL_OPENER <uri>` — launch a URI (set to the SDE launcher / `xdg-open`
  replacement). Also used for `Activate`.
- `SDE_PORTAL_NOTIFIER <id> <title> <body> <icon>` — show a notification (set to
  the Serenity `notify` tool).
- `SDE_PORTAL_FILE_CHOOSER` — the LibGUI FileChooserDialog app.
- `SDE_PORTAL_PRINT_DIALOG` / `SDE_PORTAL_PRINT_CMD` — the Print dialog and the
  command that receives the document fd.
- `SDE_PORTAL_CHOOSER` — the ScreenCast source picker.
- `SDE_PORTAL_ACCESS_DIALOG` — the permission prompt.
- `SDE_PORTAL_APP_CHOOSER` — the application chooser.
- `SDE_PORTAL_COLOR_PICKER` — the color picker.
- `SDE_PORTAL_SCREENSHOT_OVERLAY` — the interactive area selector.
- `SDE_STATE_DIR` — where wallpaper/shortcut/secret/restore-token state lives.

## 4. Install & enable

```sh
# Backend binary (built by scripts/build-tools.sh):
build/tools/xdg-desktop-portal-serenity &

# Tell the frontend about it:
install -Dm644 contrib/xdg-desktop-portal/serenity.portal \
    ~/.local/share/xdg-desktop-portal/portals/serenity.portal
install -Dm644 contrib/xdg-desktop-portal/org.freedesktop.impl.portal.desktop.serenity.service \
    ~/.local/share/dbus-1/services/org.freedesktop.impl.portal.desktop.serenity.service
install -Dm644 contrib/xdg-desktop-portal/portals.conf \
    ~/.config/xdg-desktop-portal/portals.conf
```

The D-Bus service file lets the frontend activate the backend on demand (install
the binary somewhere on `PATH`); the session also starts it directly.

`UseIn=Serenity` gates the backend on `XDG_CURRENT_DESKTOP=Serenity`; the session
sets that. With the mapping above, e.g.
`gdbus call --session --dest org.freedesktop.portal.Desktop --object-path
/org/freedesktop/portal/desktop --method org.freedesktop.portal.Settings.Read
org.freedesktop.appearance color-scheme` is served by this backend.

## 5. Phase 2b (remaining)

To reach and surpass `xdg-desktop-portal-kde`:

- **ScreenCast / RemoteDesktop** — monitor + window sources, hidden/embedded
  cursor, `pipewire-serial` (v6), restore tokens and virtual-pointer/keyboard
  input injection all landed. Remaining: **DMA-BUF** (zero-copy) and the
  **metadata** cursor mode (`ext_image_copy_capture_cursor_session_v1` +
  `SPA_META_Cursor`).
- **InputCapture** — needs compositor input routing or libei; still the `gtk`
  fallback.
- **Device**/**Camera** (v2) — device brokers and the camera PipeWire path
  (USB listing is in place; Camera needs a PipeWire camera producer).
- A **permission store** integration and the `Request`/`Response`
  choreography.

## 6. Test

`scripts/run-portal-test.sh` starts the backend on a private bus and queries every
Phase-1 interface directly (Settings, Account, Lockdown, Background, AppChooser,
ProxyResolver, OpenURI, Notification), then — best effort — proves the real
`xdg-desktop-portal` frontend routes `Settings` to this backend.

`scripts/run-screencast-test.sh` starts a headless SDE compositor and the backend,
runs a ScreenCast session (CreateSession → SelectSources → Start), asserts the
returned stream node id, that the PipeWire node `sde-screencast` is published, and
that frames are captured (a consumer pull is best-effort). It also checks the
Screenshot portal writes a PNG and that PickColor returns a color (with a stub
picker).

`scripts/run-portal-ui-test.sh` launches each LibGUI dialog under a headless
compositor, screenshots it and asserts the client area rendered (a blank-dialog
regression guard).
