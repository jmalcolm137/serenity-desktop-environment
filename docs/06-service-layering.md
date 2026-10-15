# Service Layering: libgui-wayland vs. the SDE Session

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`04-known-gaps.md`](04-known-gaps.md) · [`07-systemserver-roles.md`](07-systemserver-roles.md)

This records a principle we keep coming back to, so we don't re-litigate it:
**which side of the libgui-wayland / SDE boundary does a Serenity service belong
on?**

---

## 1. The principle

- **`libgui-wayland` is a self-contained, compositor-agnostic client stack.**
  A LibGUI/LibWindowServer application must run under **any** Wayland compositor
  with no SDE present. Every Serenity service it needs therefore has an
  **in-process fallback**. This is already the shape: `LibWM` stands up
  in-process portal servers (WindowServer, clipboard, config, launch,
  filesystem-access) over socketpairs.

- **SDE is the session.** Services that are inherently **session-scoped** (a
  shared handler database, seat/input, notifications, applets) belong in the
  compositor/session, **not duplicated inside every application process**.

- **Resolution rule:** *prefer the session service when it is present, otherwise
  fall back to the in-process one.*

> **Naming note: two kinds of "portal".** The paths above are **SerenityOS**
> portals: SDE's own IPC endpoints under `/tmp/session/<id>/portal/…`. They are
> unrelated to the freedesktop **`xdg-desktop-portal`** D-Bus service
> (FileChooser, Screenshot, ScreenCast, RemoteDesktop, InputCapture,
> GlobalShortcuts, Settings). When this document says "portal" it means the
> former; `xdg-desktop-portal` is always named in full. See
> [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §2.1.

## 2. How the rule is already plumbed

`LibWM` intercepts connects to well-known portal paths and maps them onto
in-process socketpairs (`LibWM::make_portal()`):

```
/tmp/portal/window
/tmp/session/<id>/portal/{clipboard,launch,config,filesystemaccess,image}
```

A session service can take over one of those paths by listening there itself;
`make_portal()` then **passes the client through** to it instead of the
in-process fallback. That is the single hook the rule needs.

Some services are never served in-process because they are too heavy to embed in
every application: `LibWM` **spawns** them from the session app directory on
demand (`WebContent`, `RequestServer`, `SQLServer`, `NotificationServer`, and the
SDE's **`AudioServer`**, which serves the audio + audiomanager portals from one
process). Those reach the client the same way: the app connects to the portal,
`LibWM` spawns the service and hands it the listening socket.

## 3. Applied to `LaunchServer`

- **`libgui-wayland` (fallback):** the in-process `LaunchServerConnection` keeps
  the behaviour it has always had: resolve handlers from
  `/etc/LaunchServer.ini` `[FileType]`/`[Protocol]`, plus the editor as a generic
  alternate, and spawn the matching binary. It must stay able to open files with
  the compositor-agnostic guarantee intact.
- **SDE (session service):** a `LaunchServer` that owns the launch portal when
  SDE is running and does the **richer** resolution: the `.af` `AppFile`
  launcher metadata (`[Launcher] FileTypes=` / `MimeTypes=` / `Protocols=`),
  mime types, and later user preferences and the allow-list.
- **One implementation, two hosts.** To avoid duplicating the resolution, the
  logic lives in a **dependency-light** shared component (a `LibCore`-only
  library, `LibLaunch`) that `LibDesktop::AppFile`, the in-process fallback and
  the SDE `LaunchServer` all use.

  It specifically **cannot** live in `LibDesktop` as-is: `AppFile.h` pulls in
  `LibGUI` (for `icon()`), and `LibGUI` depends on `libwindowserver`, so a
  dependency on `LibDesktop` from the shim would be a build-order cycle.

## 4. `ImageDecoder` (both hosts)

SerenityOS ships an `ImageDecoder` **service**
(`Userland/Services/ImageDecoder/`) that apps reach over
`/tmp/session/<id>/portal/image`. It is a *Serenity system service*: `SystemServer`
spawns it and hands it its client socket, and it serves exactly **one client per
process**: its `die()` quits the event loop and the background thread when that
client goes away. Under SDE there is no `SystemServer`, so we host the same
implementation class (`ImageDecoder::ConnectionFromClient`) ourselves in **both**
roles:

- **libgui-wayland (fallback):** `LibWM` stands up a server-side decoder on the
  `image` socketpair, so an application decodes images with no SDE and no external
  service, same as the other portals. The class assumes a single-client service
  process, so the in-process host sets two opt-outs (see patch `0022`):
  - `set_exit_when_client_disconnects(false)`: a client disconnecting must not
    quit the **host application's** event loop and background thread.
  - `set_decode_on_connection_thread(true)`: decode inline on the connection's
    own (LibWM portal) thread. The upstream decode job runs on
    `Threading::BackgroundAction`, a **single process-global thread**; LibGUI's
    `FileSystemModel` renders thumbnails *on that same thread* and then blocks in
    `send_sync` waiting for the decode, so sharing it deadlocked thumbnails. A
    separate process has its own background thread and never hits this.
- **SDE (session service):** `ImageDecoder/` (built by
  `scripts/build-imagedecoder.sh`) listens on the image portal when SDE runs;
  `LibWM::make_portal()` then defers to it. As a separate process it keeps the
  class's normal background decode job and needs only
  `set_exit_when_client_disconnects(false)` (it serves many clients).

"One implementation, two hosts": both reuse SerenityOS's `ConnectionFromClient`;
neither forks the decoder.

## 5. Why do this now

- The `.af` handler database, seat management, notifications and the like all
  want **one owner**; doing it now avoids baking more per-app duplication into
  the shim.
- The fallback rule preserves the compositor-agnostic guarantee, and the same
  "session service, else in-process" shape will be reused for the next service.

## 6. Constraints

- The in-process fallback must keep working **unchanged** for stand-alone use:
  no regression. Cover it with a headless test, with and without an external
  `LaunchServer` present.
- Prefer a **lean SDE `LaunchServer`** that reuses the shared resolver over
  pulling in the whole SerenityOS service (deps, allow-list, user preferences).

## 7. Status

- **`LibLaunch`** (the shared resolver, `Launch::HandlerDatabase`) lives in the
  Serenity tree (LibCore + LibURL only) and is used by libgui-wayland's
  in-process fallback.
- The **SDE `LaunchServer`** service (`LaunchServer/`, built by
  `scripts/build-launch-server.sh`) owns the launch portal when it runs; it is
  started by `scripts/start-sde-session.sh` and reuses the same protocol
  implementation (libgui-wayland's `LaunchServerConnection`, compiled in; it
  pulls no LibGUI).
- `LibWM::make_portal()` **defers to a session service** when a socket exists on
  the portal path, otherwise serves in-process. Verified: an application under
  SDE connects to the session `LaunchServer` instead of the fallback.
- The **SDE `ImageDecoder`** service (`ImageDecoder/`, built by
  `scripts/build-imagedecoder.sh`) likewise owns the image portal when it runs,
  reusing SerenityOS's `ImageDecoder::ConnectionFromClient`; without it,
  libgui-wayland's in-process decoder answers. Verified both ways.
- **Stand-alone (no SDE) is unchanged**: the in-process fallback answers.

Covered by a headless test: `scripts/run-services-test.sh` runs the same client
(the SDE's `libwm-services-test`, `test/`) twice, once with no external service and
once with the SDE `LaunchServer` and `ImageDecoder` running, and asserts that the
in-process fallback answers in the first case and LibWM **defers** to the services
in the second, for both the launch and image portals. It needs no compositor and
uses a private session directory (`SDE_SESSION_DIR`, patch `0023`) so it never
touches a live session.
