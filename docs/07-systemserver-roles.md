# SystemServer Roles, and What Stands In For Them

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`04-known-gaps.md`](04-known-gaps.md) · [`06-service-layering.md`](06-service-layering.md) · [`06-compositor-gaps.md`](06-compositor-gaps.md)

SerenityOS leans on **`SystemServer`** (the userland manager) to launch and
supervise *system services* and to own the per-session runtime. We have no
SystemServer: under SDE a session is the `WindowServer` (compositor) process plus
whatever `scripts/start-sde-session.sh` starts, and under `libgui-wayland` alone
there is only the application process. This document collects every place where we
stand in for a SystemServer role — so the substitutes stay deliberate, and so that
on a real Linux session each one can be handed to **systemd** (`systemd --user`)
instead.

---

## 1. What SystemServer does (SerenityOS)

- Spawns the services declared by `serenity_component(...)` (and, when `REQUIRED`,
  ensures they are up).
- Hands each service the socket(s) it serves: a service owns a *name* — its portal
  path in the session runtime directory.
- Owns the per-session runtime directory `/tmp/session/<sid>/portal/…`.
- Honours the dependency graph (`serenity_component(... DEPENDS ImageDecoder
  RequestServer)`).
- Restarts crashed services and applies per-service confinement
  (`pledge`/`unveil`).
- With `init`, provides the filesystem roots (`/res`, `/etc`, `/tmp`, `/bin`).

## 2. Our substitutes

### 2.1 Per-application portal servers — libgui-wayland

`LibWM` intercepts connects to the well-known portal paths and serves them
**in-process** (`LibWM::make_portal()` via `Core::PortalServer`). This is the
compositor-agnostic fallback, so an application works with no session at all:

| Portal path | SerenityOS service |
|---|---|
| `/tmp/portal/window` | WindowServer |
| `/tmp/portal/wm` | WindowServer (WindowManagerServer) |
| `/tmp/session/%sid/portal/clipboard` | Clipboard |
| `/tmp/session/%sid/portal/config` | ConfigServer |
| `/tmp/session/%sid/portal/launch` | LaunchServer |
| `/tmp/session/%sid/portal/filesystemaccess` | FileSystemAccessServer |
| `/tmp/session/%sid/portal/image` | ImageDecoder |
| `/tmp/session/%sid/portal/webcontent` | WebContent (spawned on demand; §2.3) |
| `/tmp/session/%sid/portal/request` | RequestServer (spawned on demand; §2.3) |

### 2.2 Session services — SDE

When a session is desired, `scripts/start-sde-session.sh` starts the *session
services* before the compositor:

- `LaunchServer/` — owns the launch portal.
- `ImageDecoder/` — owns the image portal.

A session service "takes over" a portal simply by **listening on its path**:
`LibWM::make_portal()` then defers the client to it instead of the in-process
fallback (the rule in [`06-service-layering.md`](06-service-layering.md)).

### 2.3 On-demand single-client services — WebContent, RequestServer

SerenityOS runs a number of services as **one process per client**:
`WebContent` (one per page/view) and `RequestServer`. SystemServer spawns a fresh
process per connection and passes it the accepted socket. We reproduce this
without a supervisor: `LibWM::spawn_single_client_service()` spawns
`<app-bin>/<Service>` on demand and passes it one end of a socketpair through the
*same* env contract SystemServer uses — `SOCKET_TAKEOVER="<name>:<fd>"`, consumed
by `Core::take_over_socket_from_system_server()`
(`serenity/Userland/Libraries/LibCore/SystemServerTakeover.cpp`). A nested spawn
(WebContent spawning RequestServer) saves and restores the parent's takeover so
neither is clobbered.

The clients connect to `/tmp/session/%sid/portal/{webcontent,request}`; `LibWM`
recognises those paths and **spawns** rather than serving in-process — LibWeb and
the HTTP/TLS stack are far too heavy to link into every application. WebContent
itself links the LibWM shim so *its* outgoing portal connects (RequestServer,
ImageDecoder) are served the same way.

The SerenityOS man pages that `Help` renders live under `Base/usr/share/man`, which
`Core::System::resolve_resource_path()` relocates (patch `0017`), alongside the
`/res` root; the tree is part of the sparse checkout.

> **Status:** done. `WebContent` and `RequestServer` are built by
> `libgui-wayland` (`src/CMakeLists.txt`) and spawned on demand. Verified: `Help`
> renders the man pages (`Audio-subsystem(7) - Help`, …) with both services
> running.

### 2.4 The WindowManagerServer

SerenityOS's WindowServer serves the WindowManagerServer at `/tmp/portal/wm`. Ours
is served by the **compositor** (`WindowServer/src/main.cpp`), which also relays
window events to the Taskbar. `SDE_WM_PORTAL_PATH` relocates the socket (the
headless tests use it so they cannot disturb a live session).

### 2.5 Session runtime directory, `/res`, `/etc`, `/bin`

- `/tmp/session/%sid/portal/…` is computed by
  `Core::SessionManagement::parse_path_with_sid()`. On Linux `root_session_id()`
  is always **0**, so `%sid` → `/tmp/session/0`; the `SDE_SESSION_DIR` seam
  relocates the whole directory (used by the tests so they never touch a live
  session).
- The SerenityOS resource tree (`/res`, `/etc`, `/bin`) is redirected to a host
  tree via `SERENITY_RES_ROOT` / `LIBWM_DEFAULT_RES`, and `/bin/X` handler paths
  map to host binaries (`Process::resolve_executable_path`, `SDE_APP_BIN_DIR`).

## 3. The systemd mapping

If SDE is packaged for a systemd host, the substitutes above map onto
`systemd --user` units:

| Our substitute | systemd analogue |
|---|---|
| `start-sde-session.sh` session services | a `sde-session.target` with one `.service` per service (`Wants=`, `After=`) |
| per-connection single-client spawn (WebContent) | a `.socket` unit with **`Accept=yes`** (one service instance per connection; the accepted fd is passed as `$LISTEN_FDS`) |
| long-running session service (LaunchServer, ImageDecoder) | a `.socket` unit with `Accept=no` plus its `.service` |
| session runtime dir `/tmp/session/%sid` | `RuntimeDirectory=` / `$XDG_RUNTIME_DIR`, `tmpfiles.d` |
| `serenity_component(... DEPENDS ...)` | `Wants=` / `Requires=` / `After=` |
| `pledge` / `unveil` confinement | systemd sandboxing (`ProtectSystem=`, `PrivateTmp=`, `NoNewPrivileges=`, …) |
| restart-on-crash | `Restart=on-failure` |
| `/res`, `/etc` roots | package install paths / `ReadOnlyPaths=` |

## 4. What we do NOT stand in for (gaps)

- **No supervisor / restart.** Nothing watches a session service; a crash is not
  restarted, and `serenity_component(... REQUIRED)` ordering is not enforced — we
  start services in script order. (See [`04-known-gaps.md`](04-known-gaps.md) R6,
  "crash recovery / watchdog".)
- **No dependency graph.** `serenity_component(... DEPENDS ...)` (e.g. `WebContent
  DEPENDS ImageDecoder RequestServer`) is advisory only: `LibWM` spawns a service
  on demand, so its dependencies must already be buildable/servable, not started
  in a supervisor's order.
- **No confinement.** `pledge`/`unveil` are no-ops on Linux.
- **Services still absent:** `SQLServer` (`IPC::MultiServer`, so a *listening*
  service rather than a per-connection spawn — `Browser` requires it for
  history/bookmarks/cookies), `WebDriver`, a real `NotificationServer` (it is a
  stub), and full `AudioServer`/`AudioManager` (audio is served by
  `libserenity-audio`).
