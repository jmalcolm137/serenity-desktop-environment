# SystemServer Roles, and What Stands In For Them

**Status:** Living document · **Date:** 2026-10-10
**Related:** [`01-objectives.md`](01-objectives.md) · [`04-known-gaps.md`](04-known-gaps.md) · [`06-service-layering.md`](06-service-layering.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md)

SerenityOS leans on **`SystemServer`** (the userland manager) to launch and
supervise *system services* and to own the per-session runtime. We have no
SystemServer: under SDE a session is the `WindowServer` (compositor) process plus
whatever `scripts/start-sde-session.sh` starts, and under `libgui-wayland` alone
there is only the application process. This document collects every place where we
stand in for a SystemServer role, so the substitutes stay deliberate, and so that
on a real Linux session each one can be handed to **systemd** (`systemd --user`)
instead.

**Principle (P9).** Every substitute we write stays **faithful to SerenityOS's
behaviour** and is built as a **standards-based Wayland/D-Bus service** that runs
under other compositors too: standard protocols, never another ecosystem's
private ones. See [`01-objectives.md`](01-objectives.md) §6 (P8/P9) and
[`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §2.3.

---

## 1. What SystemServer does (SerenityOS)

- Spawns the services declared by `serenity_component(...)` (and, when `REQUIRED`,
  ensures they are up).
- Hands each service the socket(s) it serves: a service owns a *name*, its portal
  path in the session runtime directory.
- Owns the per-session runtime directory `/tmp/session/<sid>/portal/…`.
- Honours the dependency graph (`serenity_component(... DEPENDS ImageDecoder
  RequestServer)`).
- Restarts crashed services and applies per-service confinement
  (`pledge`/`unveil`).
- With `init`, provides the filesystem roots (`/res`, `/etc`, `/tmp`, `/bin`).

## 2. Our substitutes

### 2.1 Per-application portal servers: libgui-wayland

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
| `/tmp/session/%sid/portal/sql` | SQLServer (listening socket handed over; §2.3) |

### 2.2 Session services: SDE

When a session is desired, `scripts/start-sde-session.sh` starts the *session
services* before the compositor:

- `Services/LaunchServer/`: owns the launch portal.
- `Services/ImageDecoder/`: owns the image portal.

A session service "takes over" a portal simply by **listening on its path**:
`LibWM::make_portal()` then defers the client to it instead of the in-process
fallback (the rule in [`06-service-layering.md`](06-service-layering.md)).

### 2.3 On-demand single-client services: WebContent, RequestServer

SerenityOS runs a number of services as **one process per client**:
`WebContent` (one per page/view) and `RequestServer`. SystemServer spawns a fresh
process per connection and passes it the accepted socket. We reproduce this
without a supervisor: `LibWM::spawn_single_client_service()` spawns
`<app-bin>/<Service>` on demand and passes it one end of a socketpair through the
*same* env contract SystemServer uses: `SOCKET_TAKEOVER="<name>:<fd>"`, consumed
by `Core::take_over_socket_from_system_server()`
(`serenity/Userland/Libraries/LibCore/SystemServerTakeover.cpp`). A nested spawn
(WebContent spawning RequestServer) saves and restores the parent's takeover so
neither is clobbered.

The clients connect to `/tmp/session/%sid/portal/{webcontent,request}`; `LibWM`
recognises those paths and **spawns** rather than serving in-process; LibWeb and
the HTTP/TLS stack are far too heavy to link into every application. WebContent
itself links the LibWM shim so *its* outgoing portal connects (RequestServer,
ImageDecoder) are served the same way.

The SerenityOS man pages that `Help` renders live under `Base/usr/share/man`, which
`Core::System::resolve_resource_path()` relocates (patch `0017`), alongside the
`/res` root; the tree is part of the sparse checkout.

A **multi-client** service (`IPC::MultiServer`, e.g. `SQLServer`) is different: it
accepts many clients on **one listening socket**. `LibWM::spawn_multi_client_service()`
binds and listens on the portal path itself, hands that listening fd to the service
via `SOCKET_TAKEOVER`, and then defers, so the client's real `connect()` reaches
the listener we just set up (no startup race). This is the one place the shim, not
the service, owns the listening socket, mirroring SystemServer.

> **Status:** done. `WebContent`, `RequestServer`, `SQLServer` and
> `NotificationServer` are built by **SDE** (the `Services/WebContent/`, `Services/RequestServer/`,
> `Services/SQLServer/` and `Services/NotificationServer/` projects, via `scripts/build-services.sh`):
> they are session services, so they belong with the session, not in the
> compositor-agnostic libgui-wayland client stack. The session assembles an
> **SDE-owned app directory** (`SDE_APP_BIN_DIR`, `scripts/sde-session.sh`) that
> links the port's applications and overlays these services, so the shell, the
> LaunchServer and LibWM all resolve `/bin/X` and the services from one place;
> libgui-wayland keeps only its own fallback. Verified: `Help` renders the man
> pages (`Audio-subsystem(7) - Help`, …), and `Browser` loads a local page (its
> window title becomes the page's `<title>`).

The session also seeds the SerenityOS skeleton user config
(`Base/home/anon/.config`) into the session's config directory
(`scripts/seed-serenity-home.sh`), the way SerenityOS populates a new user's home;
apps that read default configuration at startup (e.g. `Browser`'s content filters)
need it.

### 2.4 The WindowManagerServer

SerenityOS's WindowServer serves the WindowManagerServer at `/tmp/portal/wm`. Ours
is served by the **compositor** (`Services/WindowServer/src/main.cpp`), which also relays
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
- SerenityOS's CA bundle lives at `/etc/cacert.pem`; the host has none, so
  `LibTLS` falls back to the system trust store
  (`/etc/ssl/certs/ca-certificates.crt`, …), patch `0024`. DNS already uses the
  host resolver (`Core::System::getaddrinfo`).

## 3. The systemd mapping (optional)

`systemd` is **optional** (R4.3, see
[`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md));
the portable session owns ordering itself and uses `libseat` for the seat. *If*
SDE is packaged for a systemd host, the substitutes above map onto
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

- **Long-running services are supervised; on-demand services are not.**
  `scripts/sde-service-supervisor.sh` restarts LaunchServer, ImageDecoder and the
  portal backend on a crash, with a bounded budget and backoff, and stops the
  child when the session stops it (`scripts/run-service-supervisor-test.sh`).
  Single-client services (`WebContent`, `RequestServer`, the per-connection
  `NotificationServer`/`AudioServer`) are spawned on demand and are not
  long-running, so there is no process to watch; `serenity_component(... REQUIRED)`
  ordering is still not enforced (services are started in script order).
- **No dependency graph.** `serenity_component(... DEPENDS ...)` (e.g. `WebContent
  DEPENDS ImageDecoder RequestServer`) is advisory only: `LibWM` spawns a service
  on demand, so its dependencies must already be buildable/servable, not started
  in a supervisor's order.
- **No confinement.** `pledge`/`unveil` are no-ops on Linux.
- **Services still absent:** `WebDriver`, and full
  `AudioServer`/`AudioManager` (audio is served by the SDE `AudioServer`
  service, spawned on the audio portal). The
  unmodified `NotificationServer` **is** hosted (spawned on the notify portal,
  WS5).
