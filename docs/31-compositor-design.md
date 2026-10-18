# The compositor's design and invariants

**Status:** current · **Date:** 2026-10-17
**Related:** [`02-compositor-decision.md`](02-compositor-decision.md) · [`08-compositor-gaps.md`](08-compositor-gaps.md) §6 · [`25-performance.md`](25-performance.md) · [`26-ci.md`](26-ci.md)

`WindowServer/` is a wlroots compositor with one unusual design choice: it does
not run `wl_display_run()`. Wayland's event loop is pumped through a
`Core::Notifier` into Serenity's `Core::EventLoop`, so Wayland, the
WindowManagerServer IPC, timers, config watchers and signals share one loop.
Several bugs found during bring-up came from that shape, or from lifetimes that
only look different because of it. This document writes the model down so new
features do not have to rediscover it.

It describes the compositor process. The **client-side** framework
(`libgui-wayland`) has its own threads and its own design; see
[`24-libgui-wayland-sde-boundary.md`](24-libgui-wayland-sde-boundary.md).

## 1. One thread

The compositor is single-threaded. Every wlroots callback, every
WindowManagerServer message, every `Core::Timer`, every config-file watcher and
every signal handler runs on the same thread. There are no locks, and there is
no "compositor thread" to hand work to.

That is deliberate. A compositor's state is a graph of scene nodes, surfaces,
outputs and listeners, and nearly every event can destroy part of it. Serialising
all of it on one thread is what makes the ownership rules in §3 tractable. It
also makes one rule non-negotiable:

> **Never block the loop.**

Disk I/O, a synchronous wait for a client, a long `for` over surfaces, or a
`usleep` all stop input, rendering and IPC for every client at once. The
liveness heartbeat (`SDE_HEARTBEAT_FILE`) exists to make a wedged loop visible,
and it times its own write for exactly this reason: a slow file system must show
up as a slow heartbeat, not as a mystery freeze.

The *clients* are concurrent with the compositor, and the framework's
`WindowServer` shim runs a LibWM server thread inside each application process.
None of that thread is the compositor's: it must not be used to call into
`Server`. The compositor's only concurrency surface is Wayland itself.

A timer belongs to the event loop that created it. Starting or stopping one from
another thread corrupts the loop's timer index; the framework patch for LibWM's
IPC timer (`0068-libipc-timer-owner-thread`) is the same rule on the client
side. In this process, there is only one loop, so the rule is automatic — and it
is a reason not to introduce a second one casually.

## 2. The event loop

`serenity_main()` builds the server (`initialize_*`), starts the backend, then:

```cpp
Core::EventLoop loop;
auto* wayland_loop = wl_display_get_event_loop(server->display);
auto display_notifier = Core::Notifier::construct(wl_event_loop_get_fd(wayland_loop), Core::Notifier::Type::Read);
display_notifier->on_activation = [&] {
    wl_event_loop_dispatch(wayland_loop, 0);
    wl_display_flush_clients(server->display);
};
// ... timers, watchers, IPC server ...
int result = loop.exec();
finish(*server);
```

One iteration therefore services, in whatever order `Core::EventLoop` chose:

* Wayland events — protocol requests, surface commits, input events, output
  frame callbacks. A batch can create and destroy arbitrary state.
* WindowManagerServer messages — the Taskbar and applets speak this protocol to
  the compositor over a `Core::LocalServer`; they are Core events, not Wayland
  events.
* `Core::Timer` callbacks — the heartbeat, the performance/report meter, key
  repeat, cursor highlight, animations, idle.
* Config watchers — `WindowServer.ini`, `input.ini`, `screenlayout.ini`, theme,
  fonts, wallpaper, global shortcuts. They apply a validated state and are
  expected to be idempotent.
* Signals — `SIGTERM`/`SIGINT` ask the loop to quit; a fatal signal writes the
  crash report and re-raises (§8).

Two consequences follow.

**Ordering inside a Wayland batch is not a contract.** A client may destroy a
surface immediately after committing it; an output may disappear while a layer
surface is being arranged. Code must not assume that an object it saw at the top
of a callback still exists at the bottom.

**All work is visible to all clients.** A long callback delays the next frame
and the next client's input. Split large work, defer it with
`Core::deferred_invoke`, or do it on an idle frame. There is no helper thread to
move it to by default.

`spawn_startup_command()` is the one place the compositor forks. Each
`&`-separated startup command gets its own `sh -c` **foreground** job, with
`SIGINT`/`SIGQUIT` reset to default: a single `sh -c "A & B"` would make the
children inherit ignored signals, which broke Ctrl-C in a Terminal started by
the session. The compositor does not wait for these children; the session
supervisor owns their lifecycle.

## 3. Ownership and lifetimes

`Server` groups state by subsystem (`InputState`, `XWaylandState`,
`OutputManagementState`, `LockState`, …). Each group's wlroots objects and
listeners are created by that subsystem's `initialize_*()` and unhooked by its
`finish_*()`. `remove_listener()` is idempotent, so a `finish_*()` can unhook
everything its state holds whether or not each listener was wired.

The rules that matter:

* **Remove listeners before their object is destroyed.** wlroots asserts that an
  object's signal list is empty when it destroys the object. Every object this
  codebase adds a listener to is tracked in a `wl_listener` field and removed in
  the matching destroy handler or `finish_*()`. A leaked listener is an abort at
  shutdown, not a warning.
* **A callback may destroy the object it is about.** A toplevel destroy handler
  removes it from `server.toplevels`; a surface destroy listener removes its
  scene node; a client disconnect can remove several things. Do not keep a raw
  pointer across a call that can deliver events. Use `NonnullRefPtr` protection
  (as the framework's IPC connections do) or re-resolve after the call.
* **Shutdown is reverse initialization, clients first.** `finish()` destroys
  clients while surfaces and scene nodes still exist, unhooks subsystems in
  reverse order, releases the compositor's cached overlay nodes, then destroys
  the backend, scene, cursor, allocator, renderer and display. The backend goes
  before the scene because destroying it destroys outputs. `WindowServer` must
  never `exit()` from a callback: signal handlers ask the loop to quit so this
  order runs.
* **Overlay `ChromeNode`s belong to the scene, not to static storage.** The
  window menu, switcher, animations and cursor highlight cache nodes; their
  destructors would otherwise run after `wlr_scene_node_destroy()` and touch
  freed memory. `finish()` clears them explicitly before the scene
  (`2d39666`).
* **The window lists are the truth.** There are two lists, `toplevels` (xdg) and
  `x11_toplevels` (X11). Focus, the switcher, the Taskbar, output hotplug and
  the window menu walk both. A feature that only updates `toplevels` is
  incomplete by construction.
* **Client-owned resources are borrowed.** A `serenity_window_manager` resource
  or a protocol object may go away with its client at any time; the compositor
  tracks and removes it in the resource destroy handler and never dereferences
  it afterwards.

## 4. The scene and the render model

`Server::scene` is a `wlr_scene` with six trees, bottom-to-top:

```
background   wallpaper / Desktop
bottom       layer-shell bottom
windows      normal toplevels and X11 windows
top          layer-shell top (Taskbar, applets)
overlay      session lock, window menu, switcher, animations
drag_icon    drag icons (above everything)
```

Layer surfaces are configured by `arrange_layers()`, which walks the four
layer-shell layers, accumulates exclusive zones, updates each output's usable
area and then re-applies the applet-area layout. Windows are placed in **layout
coordinates** (the union of the outputs), not output-local ones; this is why an
output disappearing requires moving windows, not just hiding them.

Rendering is damage-driven. `output_frame()` calls
`wlr_scene_output_commit()` and `wlr_scene_output_send_frame_done()` for the
output that fired; wlr_scene decides what to paint. With no damage, no frame is
committed — the performance test asserts "no frames while idle". Never render
from a timer or poll for damage.

The frame callback is also where:

* the performance meter records frame time and input-to-present latency;
* presentation-time feedback is sent;
* a pending **screen capture** request is served, from the buffer that was just
  presented (`Capture.cpp`).

Capture is deliberately one frame behind: the shim has no synchronous Wayland
request, so the compositor queues the region and answers on the next commit
(`wlr_texture_from_buffer` + `wlr_texture_read_pixels`). This reads exactly what
the user sees, works for DMA-BUF and SHM, and never blocks the frame. A capture
request whose manager resource disappears is dropped.

HiDPI is applied at the output (`wlr_output_state_set_scale` and
`wp-fractional-scale-v1`); the compositor's own chrome is drawn at an integer
supersample and filtered for fractional scales. Client buffers are rastered at
the scale the scene assigns them. Do not hardcode pixel sizes for chrome: use
the logical output size and let the scene/filter handle the device pixels.

## 5. Focus, input and the lock

There is exactly one active window. `focus_toplevel()` and
`focus_x11_toplevel()` call `deactivate_other_windows()` rather than trusting the
seat's previous focused surface, because the seat can have no focused surface
(headless, first window of a session, a layer menu taking focus). The change is
reported to the WindowManagerServer so the Taskbar button follows.

Keyboard focus has two levels:

* the **seat's** focused surface — what wlroots delivers keys to;
* the compositor's **frame-active** state — what the titlebar, switcher and
  Taskbar draw.

They are kept in step by the focus functions, but they are not the same value:
a layer surface with `keyboard_interactivity=on_demand` can hold the seat focus
without being an active window, and a lock surface holds it while locked.
`server.keyboard_focus_layer` records the on-demand layer so that unmapping it
restores focus to the topmost window.

**Keyboard hotplug is safe by construction.** When the last keyboard goes away,
`update_seat_capabilities()` withdraws `WL_SEAT_CAPABILITY_KEYBOARD`; wlroots
keeps the focused client/surface and re-sends `enter` when the client acquires a
new `wl_keyboard`. The compositor does not clear or re-activate a window on a
replug. `run-keyboard-hotplug-test.sh` asserts both the re-enter and a working
key.

**Input goes to the lock, never to normal clients, while locked.**
`ext-session-lock-v1` surfaces are not a layer: they are configured for every
output and the compositor must not send `locked` until every output is covered.
A size or scale change reconfigures the lock surface for that output; the lock
test changes the mode while locked and asserts the re-configure. While locked,
the keyboard goes to a lock surface, key repeat stops, and IME focus is cleared.

Input injection for tests uses wlroots' virtual keyboard/pointer plus the
SDE-only `serenity-virtual-input` protocol (touch/tablet). That protocol exists
only for the headless harness; it is not a product input path.

## 6. IPC and the session's other processes

The **WindowServer protocol** (the app-facing one) is not served by this
process. Each Serenity application links libgui-wayland's `WindowServer` shim,
which implements the protocol in-process on the client side. The compositor sees
only Wayland: surfaces, commits, xdg-shell/layer-shell, and the custom
`serenity-window` / `serenity-window-manager` protocols. That split is what lets
unmodified LibGUI applications run as ordinary Wayland clients
([`24-libgui-wayland-sde-boundary.md`](24-libgui-wayland-sde-boundary.md)).

The **WindowManagerServer** *is* served here: `Core::LocalServer` on
`/tmp/portal/wm` (or `SDE_WM_PORTAL_PATH`), accepted in the same loop, carrying
window state, workspace and applet-area events to the Taskbar and applets. It is
a push protocol: the compositor emits state; the Taskbar reacts. There is no
synchronous request/reply between them on the hot path.

The session's other processes are separate and have their own lifecycle:

* `LaunchServer`, `ImageDecoder` and the portal backend are long-running
  services, supervised with a restart budget (`sde-service-supervisor.sh`).
* `WebContent`, `RequestServer`, `NotificationServer` and `AudioServer` are
  spawned on demand by the client-side framework, per connection.
* The Taskbar is the shell; `sde-shell.sh` restarts it if it exits and falls
  back to a Terminal if it cannot start.
* The session supervisor (`sde-session.sh`) restarts the compositor on a fault
  or a wedged heartbeat, and drops to safe mode after the budget.

None of those processes may call into the compositor's `Server`. Their only
contract is their protocol and the environment (`WAYLAND_DISPLAY`,
`SDE_APP_BIN_DIR`, `SDE_WM_PORTAL_PATH`, `AT_SPI_BUS_ADDRESS`, …).

## 7. Configuration and state

`WindowServer.ini`, `input.ini`, `screenlayout.ini` and the theme/font/wallpaper
brokers are read and watched under the session state directory
(`$XDG_CONFIG_HOME/sde` by default, `SDE_STATE_DIR` for tests). A watcher's
callback runs on the loop and must be:

* **validated** — `ConfigRules` owns the ranges and enums; invalid values fall
  back to the default rather than being applied;
* **idempotent** — a file save can produce several wakeups; applying the same
  layout/scale/theme twice must be harmless;
* **bounded** — no long I/O, no waiting for a client.

The theme broker is a file, not a socket: the compositor writes the applied
theme and every client watches it. The compositor must not fight a client that
changes it (the high-contrast mode pins the broker to `Contrast` and says so).
The wallpaper state follows the same broker pattern.

## 8. Failure is a first-class path

* **A client may disappear at any time.** Disconnect is normal; its surfaces,
  listeners and WindowManagerServer entries are removed, and no other client is
  affected.
* **A protocol error is isolated.** `wl_client_post_implementation_error` (or the
  resource's error) drops that client; the compositor keeps serving and can map
  a fresh one (`run-headless-test.sh` asserts both).
* **A fatal signal writes a report and re-raises.**
  `$SDE_CRASH_LOG` gets the signal and a backtrace, then the default action runs
  so the supervisor sees a failed compositor and restarts it.
* **A wedged loop is detected from outside.** The heartbeat stops advancing; the
  supervisor restarts the compositor, and safe mode guards against a login loop.
* **Xwayland may die independently.** `update_xwayland_workareas()` checks that
  wlroots still has an XWM connection before asking it to update; an X server
  going away must not take the compositor with it.
* **Services and the shell have their own supervisors** (§6), so a crash in one
  does not leave the session without a panel, a launcher or image decoding.

Logs are an interface: the headless tests assert on compositor log lines.
`[Log] Level` / `SDE_LOG_LEVEL` selects verbosity; a message used by a test is
part of the behaviour, so changing it changes the test.

When a log line cannot reconstruct the sequence, set `SDE_TRACE_FILE`. The
compositor then writes one JSON object per line (`t`, `cat`, `msg`) for window
map/unmap/destroy, focus, workspace, output and WindowManagerServer events
(`Trace.cpp`, `scripts/run-trace-test.sh`). It is synchronous and opt-in: leave
it off on a hot path, and do not add an event per frame.

## 9. Rules for new code

1. Do not block the loop. If work is large, defer or split it.
2. Do not add a thread. If you must, it owns its own event loop, timers and
   socket reads; it never touches `Server`, the scene or the seat. Timers start
   and stop only on the loop that created them.
3. Treat every callback as able to destroy the object it is about. Re-resolve or
   protect pointers; remove listeners in the destroy path.
4. Add a listener only with a matching `wl_listener` field and a matching
   removal. `remove_listener()` is idempotent; use it.
5. Tear down in reverse order of setup, clients first, overlay nodes before the
   scene.
6. Walk both window lists (`toplevels` and `x11_toplevels`) when a feature is
   about "windows", unless it is explicitly xdg-only.
7. Push state; never add a synchronous round trip to a client. A one-frame
   delay is cheaper than a blocked compositor.
8. Validate config, make watchers idempotent, and never assume a file save
   arrives once.
9. Render from damage/commits only. Capture from the committed frame. Do not
   read back the visible buffer on the hot path.
10. A client disconnect, a protocol error and a missing optional service are
    normal. Log them; do not assert on them.
11. Write the headless test with the feature. A log line the test keys off is
    part of the contract.
12. Prefer a small, named helper over a second copy of a rule (focus, usable
    area, monitor layout, applet layout, placement); the duplicated version is
    the one that goes stale.
