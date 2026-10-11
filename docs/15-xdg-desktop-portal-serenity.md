# xdg-desktop-portal-serenity (R5)

**Status:** Phase 1 landed · **Related:** [`08-compositor-gaps.md`](08-compositor-gaps.md) §3,
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

## 2. Phase 1 interfaces

| Interface | Behaviour |
|---|---|
| `Settings` (v2) | `color-scheme` (from the active theme's `Window` luminance), `accent-color` (theme `Accent`/`Selection`), `contrast` — read from the theme broker (`$XDG_RUNTIME_DIR/sde-theme`). |
| `Account` (v1) | `GetUserInformation` from the passwd entry (`name`, `id`; `image` empty). |
| `Lockdown` (v1) | Read-only properties; false by default, overridable by `SDE_PORTAL_LOCKDOWN_*`. |
| `Background` (v1) | `ApplicationName`/`ApplicationID`; `SetStatus`; `SetAutostart` writes an XDG autostart entry (the SDE supervisor honours XDG autostart). |
| `AppChooser` (v1) | `ChooseApplication` returns the first offered application (a real chooser is Phase 2). |
| `Email` (v1) | `ComposeEmail` opens a `mailto:` URI. |
| `Location` (v1) | Reports "not available" (no location provider). |
| `ProxyResolver` (v1) | `Lookup` returns an empty list (direct connection). |
| `OpenURI` (v3) | `OpenURI`/`OpenDirectory`/`OpenFile` launch via `SDE_PORTAL_OPENER` (the session launcher) or GIO's default-app launch. |
| `Notification` (v1) | `AddNotification`/`RemoveNotification` via `SDE_PORTAL_NOTIFIER` (the Serenity notify tool) or `notify-send`. |
| `ScreenCast` (v4) | `CreateSession`/`SelectSources`/`Start`/`AvailableSourceTypes`/`AvailableCursorModes`. Captures an output **or a window** with the standard **`ext-image-copy-capture-v1`** and publishes a **PipeWire** video stream (`sde-screencast`); `Start` returns the stream node id + position/size/source_type/id. Cursor modes: hidden, embedded. |
| `Realtime` (v1) | Grants high-priority/realtime threads (accepted). |
| `PowerProfileMonitor` (v1) | `power-saver-enabled` (from `SDE_POWER_SAVER`). |
| `GameMode` (v1) | `RegisterGame`/`UnregisterGame`/`QueryStatus` (reports not active). |
| `MemoryMonitor` (v1) | `LowMemoryWarning` signal; `version` only. |
| `FileChooser` (v2) | `OpenFile`/`SaveFile` (directory mode via the `directory` option) through the LibGUI **FileChooserDialog** app (`SDE_PORTAL_FILE_CHOOSER`); returns the chosen `uris`. |
| `Screenshot` (v1) | `Screenshot` captures the primary output (via the ScreenCast capture pipeline, on its own Wayland connection) into a PNG and returns its `uri`. |
| `Print` (v1) | `PreparePrint` via the LibGUI **PrintDialog** app (`SDE_PORTAL_PRINT_DIALOG`; printers from `lpstat`); `Print` feeds the document fd to `SDE_PORTAL_PRINT_CMD` (e.g. `lp`). |
| `RemoteDesktop` (v2) | `CreateSession`/`SelectDevices`/`Start` (devices + the paired ScreenCast stream) and the `Notify*` input methods — pointer motion/absolute/button/axis and keyboard keycodes — injected via `wlr-virtual-pointer` / `virtual-keyboard`. `ConnectToEIS` (libei) is not supported, so apps use the `Notify*` methods. |

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

Still to add (niri parity, Phase 2b): **DMA-BUF** capture/negotiation (zero-copy),
the **metadata** cursor mode (`ext_image_copy_capture_cursor_session_v1` +
`SPA_META_Cursor`), the `pipewire-serial` (v6) property, restore tokens, and
niri-style extras (dynamic cast target, "block out from screencast").

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
(`scripts/build-print-dialog.sh`).

## 3. SDE integration seams

Two environment variables let the session point the backend at SDE components
without hard-coding them (the backend stays portable):

- `SDE_PORTAL_OPENER <uri>` — launch a URI (set to the SDE launcher / `xdg-open`
  replacement).
- `SDE_PORTAL_NOTIFIER <title> <body>` — show a notification (set to the Serenity
  `notify` tool).

## 4. Install & enable

```sh
# Backend binary (built by scripts/build-tools.sh):
build/tools/xdg-desktop-portal-serenity &

# Tell the frontend about it:
install -Dm644 contrib/xdg-desktop-portal/serenity.portal \
    ~/.local/share/xdg-desktop-portal/portals/serenity.portal
install -Dm644 contrib/xdg-desktop-portal/portals.conf \
    ~/.config/xdg-desktop-portal/portals.conf
```

`UseIn=Serenity` gates the backend on `XDG_CURRENT_DESKTOP=Serenity`; the session
sets that. With the mapping above, e.g.
`gdbus call --session --dest org.freedesktop.portal.Desktop --object-path
/org/freedesktop/portal/desktop --method org.freedesktop.portal.Settings.Read
org.freedesktop.appearance color-scheme` is served by this backend.

## 5. Phase 2 (roadmap)

To reach and surpass `xdg-desktop-portal-kde`:

- **ScreenCast / RemoteDesktop / InputCapture** — ScreenCast (monitor + window,
  hidden/embedded cursor) and **RemoteDesktop** (input injection via the virtual
  pointer/keyboard) landed. Remaining: **InputCapture** (needs compositor input
  routing or libei), DMA-BUF + the metadata cursor mode, `pipewire-serial` (v6).
- **GlobalShortcuts** — compositor global-shortcut registration.
- **DynamicLauncher**, **USB**, **Device**, **Camera** (v2).
- A **permission store** integration and the `Request`/`Response` choreography for
  the interfaces that return a `handle`.

## 6. Test

`scripts/run-portal-test.sh` starts the backend on a private bus and queries every
Phase-1 interface directly (Settings, Account, Lockdown, Background, AppChooser,
ProxyResolver, OpenURI, Notification), then — best effort — proves the real
`xdg-desktop-portal` frontend routes `Settings` to this backend.

`scripts/run-screencast-test.sh` starts a headless SDE compositor and the backend,
runs a ScreenCast session (CreateSession → SelectSources → Start), asserts the
returned stream node id, that the PipeWire node `sde-screencast` is published, and
that frames are captured (a consumer pull is best-effort).
