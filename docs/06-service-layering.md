# Service Layering: libgui-wayland vs. the SDE Session

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`02-compositor-decision.md`](02-compositor-decision.md) · [`03-engineering-conventions.md`](03-engineering-conventions.md) · [`04-known-gaps.md`](04-known-gaps.md)

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

- **SDE is the session.** Services that are inherently **session-scoped** — a
  shared handler database, seat/input, notifications, applets — belong in the
  compositor/session, **not duplicated inside every application process**.

- **Resolution rule:** *prefer the session service when it is present, otherwise
  fall back to the in-process one.*

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

## 3. Applied to `LaunchServer`

- **`libgui-wayland` (fallback):** the in-process `LaunchServerConnection` keeps
  the behaviour it has always had — resolve handlers from
  `/etc/LaunchServer.ini` `[FileType]`/`[Protocol]`, plus the editor as a generic
  alternate, and spawn the matching binary. It must stay able to open files with
  the compositor-agnostic guarantee intact.
- **SDE (session service):** a `LaunchServer` that owns the launch portal when
  SDE is running and does the **richer** resolution — the `.af` `AppFile`
  launcher metadata (`[Launcher] FileTypes=` / `MimeTypes=` / `Protocols=`),
  mime types, and later user preferences and the allow-list.
- **One implementation, two hosts.** To avoid duplicating the resolution, the
  logic lives in a **dependency-light** shared component (a `LibCore`-only
  library, `LibLaunch`) that `LibDesktop::AppFile`, the in-process fallback and
  the SDE `LaunchServer` all use.

  It specifically **cannot** live in `LibDesktop` as-is: `AppFile.h` pulls in
  `LibGUI` (for `icon()`), and `LibGUI` depends on `libwindowserver`, so a
  dependency on `LibDesktop` from the shim would be a build-order cycle.

## 4. `ImageDecoder`: in-process only

SerenityOS already ships an `ImageDecoder` **service**
(`Userland/Services/ImageDecoder/`) that apps reach over
`/tmp/session/<id>/portal/image`. It is a *Serenity system service*: `SystemServer`
spawns it and hands it its client socket, and it serves exactly **one client per
process** — its `die()` quits the event loop and the background thread when that
client goes away. Under SDE there is no `SystemServer`, so the upstream service
*process* cannot be used as-is.

We reuse its **implementation class**
(`ImageDecoder::ConnectionFromClient`) as libgui-wayland's **in-process fallback**:
`LibWM` stands up a server-side decoder on the `image` socketpair, so an
application decodes images with no SDE and no external service — same as the
other portals.

Unlike `LaunchServer`, we do **not** add an SDE session service for it. A session
instance would only be a sharing optimisation (one decoder instead of one per
app); it is not needed for correctness, and the upstream service does not fit the
multi-client session model without more scaffolding. The launch service, by
contrast, owns genuinely session-scoped state (the handler database, the launch
allow-list), which is why it is the one we host in the session.

The class needs a small upstreamable patch
(`patches/0022-imagedecoder-in-process-host.patch`) with two in-process opt-outs:

- `set_exit_when_client_disconnects(false)` — a client disconnecting must not
  quit the **host application's** event loop and background thread (the standalone
  service keeps the default: exit with its sole client).
- `set_decode_on_connection_thread(true)` — the upstream decode job runs on
  `Threading::BackgroundAction`, which is a **single process-global thread**. But
  LibGUI's `FileSystemModel` renders thumbnails *on that same thread* and then
  blocks in `send_sync` waiting for the decode — so a decode job could never be
  scheduled and thumbnail rendering deadlocked. Upstream never hits this because
  `ImageDecoder` is a separate process with its own background thread; an
  in-process decoder must instead decode inline on its own (LibWM portal) thread.

## 5. Why do this now

- The `.af` handler database, seat management, notifications and the like all
  want **one owner**; doing it now avoids baking more per-app duplication into
  the shim.
- The fallback rule preserves the compositor-agnostic guarantee, and the same
  "session service, else in-process" shape will be reused for the next service.

## 6. Constraints

- The in-process fallback must keep working **unchanged** for stand-alone use —
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
  implementation (libgui-wayland's `LaunchServerConnection`, compiled in — it
  pulls no LibGUI).
- `LibWM::make_portal()` **defers to a session service** when a socket exists on
  the portal path, otherwise serves in-process. Verified: an application under
  SDE connects to the session `LaunchServer` instead of the fallback.
- **Stand-alone (no SDE) is unchanged** — the in-process fallback answers.

Still to do: a headless test that exercises launching both with and without the
external `LaunchServer`.
