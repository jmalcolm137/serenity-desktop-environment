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

- **ScreenCast / RemoteDesktop / InputCapture** — `ext-image-copy-capture-v1`
  + PipeWire (`libpipewire-0.3` is available), with compositor-side input
  injection.
- **GlobalShortcuts** — compositor global-shortcut registration.
- **DynamicLauncher**, **FileChooser** (native, via FileManager), **Print**,
  **USB**, **Device**, **Realtime**, **Camera** (v2), **PowerProfileMonitor**,
  **GameMode**, **MemoryMonitor**.
- A **permission store** integration and the `Request`/`Response` choreography for
  the interfaces that return a `handle`.

## 6. Test

`scripts/run-portal-test.sh` starts the backend on a private bus and queries every
Phase-1 interface directly (Settings, Account, Lockdown, Background, AppChooser,
ProxyResolver, OpenURI, Notification), then — best effort — proves the real
`xdg-desktop-portal` frontend routes `Settings` to this backend.
