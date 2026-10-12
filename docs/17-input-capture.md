# InputCapture Portal — Design (input capture + EIS)

**Status:** **Implemented (P1–P4)** — compositor EIS implementation + private protocol + the `xdg-desktop-portal-serenity` backend · v1 · **Date:** 2026-10-12
**Related:** [`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md) §5–6 ·
[`08-compositor-gaps.md`](08-compositor-gaps.md) §3–4 ·
[`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §R5 ·
[`12-r4.3-portable-session-integration.md`](12-r4.3-portable-session-integration.md) ·
`contrib/xdg-desktop-portal/`

This document resolves the one open ScreenCast/Input problem that needed a
decision before any code could land: **`org.freedesktop.portal.InputCapture`**
([`15`](15-xdg-desktop-portal-serenity.md) §6.4, [`08`](08-compositor-gaps.md) §4).
It is a **design**, not an implementation: the portal backend still routes
`InputCapture` to `gtk` today. It records the architecture, the decision, the
protocol surface, the security model and the bring-up order.

---

## 1. Why this is different from the other portals

Every other SDE portal (ScreenCast, RemoteDesktop, Screenshot, …) either
*injects* input (RemoteDesktop, via `wlr-virtual-pointer`/`virtual-keyboard`) or
*captures pixels* (ScreenCast, via `ext-image-copy-capture-v1`). Both are
client→compositor or compositor→client **Wayland** flows.

InputCapture captures **input events themselves** and, once triggered, delivers
them to an application instead of using them to drive the on-screen pointer and
focus. That means:

- the **compositor** must hand *live physical input* to the portal, which no
  standard Wayland protocol exposes (a global input stream is deliberately not a
  thing), and
- the transport is **not** Wayland: the portal spec delegates it to **libei/EIS**
  (`org.freedesktop.portal.InputCapture.ConnectToEIS` returns a file descriptor
  to an EIS implementation).

So a design is unavoidable. The portal frontend presents the user prompt; our
backend (`xdg-desktop-portal-serenity`) must produce the EIS fd and connect it to
the compositor.

---

## 2. Actors

```
   application (libei)                xdg-desktop-portal frontend
        │  libei/EIS                        │  D-Bus org.freedesktop.portal.InputCapture
        │                                   ▼
        │                         xdg-desktop-portal-serenity (backend)
        │                                   │  D-Bus org.freedesktop.impl.portal.InputCapture
        │                                   │  + permission dialog (AccessDialog)
        │                                   ▼
        │                          WindowServer (SDE compositor)
        └────────── EIS fd ─────── (libeis: zones, barriers, enable/activate, input)
```

- **Compositor** (`WindowServer`): owns physical input, the cursor, the output
  layout (the portal's "zones") and pointer barriers. Runs the **EIS
  implementation** (`libeis`).
- **Backend** (`xdg-desktop-portal-serenity`): the D-Bus adapter. Implements
  `org.freedesktop.impl.portal.InputCapture`, drives the user prompt, relays
  session control to the compositor and returns the EIS fd to the frontend.
- **Application**: a libei client in **passive/receiver** mode, whose events come
  from the EIS implementation. It receives captured input and may inject emulated
  input back (devices it creates on the EI connection).

libei `1.6.0` is available on the target (Arch `libei`; `libei-1.0` and
`libeis-1.0` pkg-config). `liboeffis` (the client-side portal helper) is part of
the same package, so real apps (and a test client) can drive us.

---

## 3. Options considered

| # | Where the EIS implementation runs | Compositor↔backend link | Verdict |
|---|---|---|---|
| **A** | **Compositor** (`WindowServer` links `libeis`) | small private Wayland protocol; fd passed through it | **chosen** |
| B | Backend process links `libeis` | private protocol streams *all* input to the backend and injects back | rejected — see §3.2 |
| C | Keep `gtk` fallback | none | rejected for SDE (`gtk` cannot see SDE input) |

### 3.1 Option A — compositor hosts the EIS implementation (chosen)

The component that already has the input, the cursor, the output layout and the
pointer-barrier logic hosts the EIS server. This is exactly what KWin and Mutter
do. The compositor:

- creates an `eis` context with `eis_setup_backend_fd()`, calls
  `eis_backend_fd_add_client()` to get the **client end** of a private
  socketpair, and hands that fd to the portal backend (which relays it over
  D-Bus `h` to the app, which passes it to `ei_setup_backend_fd()`);
- creates one EI seat and pointer/keyboard devices that represent the logical
  (not per-device) input, and feeds real events into them
  (`eis_device_pointer_motion()`, `eis_device_button()`, `eis_device_keyboard_key()`,
  … then `eis_device_frame()`);
- reads emulated events off the EI connection and injects them into its own seat
  (the same path as `wlr-virtual-pointer`/`virtual-keyboard`);
- owns zones (outputs), pointer barriers, activation and the `activation_id`.

The **backend does no EIS work**: it is D-Bus + permission UI + fd relay.

**Why.** One source of truth for input, cursor and barriers; no marshalling of a
global input stream across a process boundary; lowest latency; matches upstream.
The project already lets the compositor link portable input libraries
(`libwlroots`, `libinput`, `libseat`); `libeis` is the same kind of portable,
init-agnostic library (P1/P4). It adds **no D-Bus and no systemd** to the
compositor: all of that stays in the backend (the "compositor stays pure" rule in
[`12`](12-r4.3-portable-session-integration.md) §2 is about system integration,
not input transport).

### 3.2 Option B — backend hosts the EIS implementation (rejected)

The backend would link `libeis` and the compositor would stream live input to it
over a private protocol, receiving emulated injection back. Rejected because:

- it needs a broad "send me every input event" compositor protocol — a larger and
  more dangerous surface than "hand me an EIS fd";
- zones/barriers/activation are still compositor decisions, so the logic splits
  across two processes for no benefit;
- every event crosses two process boundaries, adding latency to the exact path
  (input) where it hurts most.

The only argument for B — keeping `libeis` out of the compositor — is weaker than
the correctness/latency cost, since `libeis` is portable and the compositor
already owns input.

---

## 4. What the backend must implement (`org.freedesktop.impl.portal.InputCapture`, v2)

Mirrors the app-facing interface. Every method is a thin mapping onto the private
protocol (`§5`), except the prompt.

| Method / signal | Behaviour |
|---|---|
| `SupportedCapabilities` | `POINTER \| KEYBOARD` (`1\|2 = 3`). No `TOUCHSCREEN` (SDE has no touch, [`08`](08-compositor-gaps.md) §4). |
| `CreateSession` (deprecated) / `CreateSession2` | allocate a session; `CreateSession2` returns a not-yet-started session. |
| `Start` | show the **AccessDialog** ("Allow *<app>* to capture input?" via `SDE_PORTAL_ACCESS_DIALOG`); on allow, return the granted `capabilities` (subset of requested) and, if `persist_mode != 0`, a `restore_data` token (reuse the ScreenCast/RemoteDesktop token store). `clipboard_enabled` is always `false` (clipboard capture is out of scope, §8). |
| `GetZones` | compositor returns the session's zones — `a(uuii)` `(width,height,x,y)` from the output layout — plus an incrementing `zone_set`. |
| `SetPointerBarriers` | compositor stores the barriers (edge lines) for the session; returns `failed_barriers` for any rejected (off-zone / diagonal) barrier. |
| `Enable` / `Disable` | enable/disable triggering for the session; on activation the compositor emits `Activated`, on stop `Deactivated`. |
| `Release` | stop an ongoing activation (`activation_id` + suggested `cursor_position`). |
| `ConnectToEIS` | ask the compositor for a session EIS fd and return it (`h`). Must be called before `Enable`; the same connection survives `Disable`/`Enable`. |
| `Disabled` / `Activated` / `Deactivated` / `ZonesChanged` signals | forwarded from the compositor (with `activation_id`, `cursor_position`, `barrier_id`, `zone_set` as specified). |

`version = 2`.

---

## 5. Private compositor protocol (`serenity_input_capture`)

A small, **privileged** Wayland protocol (new `WindowServer/protocol/…` plus
generated bindings, like `serenity-window`). It is only offered when the session
opts in (`SDE_INPUT_CAPTURE=1`, §6). Sketch:

```
interface serenity_input_capture_manager_v1
  create_session(app_id) -> serenity_input_capture_session_v1

interface serenity_input_capture_session_v1
  connect_to_eis()                 -> event eis_fd(fd)      # eis_backend_fd_add_client()
  get_zones()                      -> event zones(zones[], zone_set)
  set_pointer_barriers(barriers[], zone_set) -> event barriers_result(failed_ids[])
  enable(capabilities)
  disable()
  release(activation_id, cursor_x, cursor_y)
  destroy()

  events:
    zones(zones[], zone_set)
    zones_changed()
    activated(activation_id, cursor_x, cursor_y, barrier_id)
    deactivated(activation_id, cursor_x, cursor_y)
    disabled()
```

Compositor behaviour:

- **Zones** = the logical outputs (`wlr_output_layout` boxes). `zone_set`
  increments whenever the output set/geometry changes; `ZonesChanged` is emitted.
- **Barriers** are validated to lie on a zone edge and be axis-aligned; invalid
  ones come back in `barriers_result`. Setting barriers suspends any active
  capture (spec) until `enable` is called again.
- **Enable** arms triggering. While armed, the compositor watches pointer motion;
  when the pointer would cross a barrier, it **activates**: emits `activated`
  (with an incrementing `activation_id`, the barrier id and the logical cursor
  position) and calls `eis_device_start_emulating(device, activation_id)` on the
  pointer/keyboard devices **before** the first event — the spec requires the
  EIS `sequence` to equal `activation_id`.
- **While active**, physical pointer (and, if granted, keyboard) events are fed to
  the EIS devices and are **not** delivered to the focused Wayland client. (A
  libei *receiver* context only receives events; the app does not inject. Its only
  feedback is `Release(cursor_position)`.)
- **Disable/Release/Deactivate** stops feeding, calls
  `eis_device_stop_emulating()`, honours the suggested `cursor_position`, and
  resumes normal input routing.
- The EIS fd is registered with the compositor's existing event loop (the
  `Core::Notifier` + `Core::EventLoop` model, [`08`](08-compositor-gaps.md) §6);
  `eis_dispatch()` pumps the socket and `eis_get_event()` yields client events.

---

## 6. Security model

InputCapture is a **keylogger-class** capability, so:

- The private global is **not advertised by default**. The session enables it
  (`SDE_INPUT_CAPTURE=1`) only when the SDE portal backend is the trusted client;
  when disabled, the global is absent and no client can capture input.
- The **user-visible gate** is the portal prompt (the backend's `Start` →
  `AccessDialog`). No session may `Enable` without a granted `Start`.
- Only the granted **capabilities** are ever emitted (a pointer-only session
  never receives keyboard events).
- The compositor keeps an **escape hatch**: an active capture can always be
  released by the compositor (e.g. a reserved keybinding), so a misbehaving app
  cannot lock the user out.
- The EIS fd is per-session (`eis_setup_backend_fd` + socketpair), not a shared
  socket, so there is no unauthenticated-connection race
  (`08-compositor-gaps.md` §6 notes the loop model; no new trust boundary).

---

## 7. Bring-up order

1. **P1 — fd handoff (landed).** `WindowServer` links `libeis` (optional) and
   offers the `serenity_input_capture` global when `SDE_INPUT_CAPTURE=1`;
   `create_session` + `connect_to_eis` create the EIS implementation
   (`eis_setup_backend_fd`, pumped from the compositor event loop) and return a
   per-session fd. Covered by `scripts/run-input-capture-test.sh`. The D-Bus
   `ConnectToEIS` *relay* lands with the backend interface in P4.
2. **P2 — pointer zones, barriers, activation (landed).** `get_zones` from the
   output layout (with `zones_changed` on output changes); `set_pointer_barriers`
   (axis-aligned; invalid ones rejected); `enable`/`disable`/`release`; a barrier
   crossing emits `activated` and feeds pointer motion/buttons over EIS until
   release. Covered by `scripts/run-input-capture-test.sh`. (A libei receiver
   context only receives — there is no app→compositor injection to wire.)
3. **P3 — keyboard (landed).** The EIS seat advertises KEYBOARD and a virtual
   keyboard device is created with the configured XKB keymap
   (`eis_device_new_keymap`); while a keyboard-capturing session is active, key
   events and the XKB modifier state are fed over EIS. Covered by the same test.
4. **P4 — D-Bus end-to-end (landed).** `tools/portal-input-capture.{h,c}` implements
   `org.freedesktop.impl.portal.InputCapture` in `xdg-desktop-portal-serenity`:
   session/receiving relay to the compositor over the private protocol, the EIS fd
   from `ConnectToEIS`, the `Activated`/`Deactivated`/`Disabled`/`ZonesChanged`
   signals, the `AccessDialog` prompt, and `portals.conf` now prefers `serenity`
   (also added to `serenity.portal`). Covered by
   `scripts/run-input-capture-portal-test.sh`.
5. **P5 — persistence.** `persist_mode`/`restore_data` and `ZonesChanged`
   re-arming (reuse the ScreenCast restore-token store).

Each phase is independently testable; P1–P3 are headless (the compositor's
`wlr-virtual-pointer`/`virtual-keyboard` can script a barrier crossing without a
GPU), so CI stays green.

---

## 8. Exit tests

- `scripts/run-input-capture-test.sh` (headless; **P1–P3 landed**): starts the
  compositor with `SDE_INPUT_CAPTURE=1`; `tools/input-capture-test.c` creates a
  session, connects the libei receiver, requests zones, installs a valid and an
  invalid barrier, arms capture (keyboard + pointer), and the test moves a virtual
  pointer across the barrier and types a key. Asserts: a seat and both devices are
  negotiated; the zones are the output (1280x720 at 0,0); the diagonal barrier is
  rejected; crossing emits `activated` (with the barrier id and activation_id) and
  pointer motion arrives over EIS; and a key press/release arrives over EIS.
- `scripts/run-input-capture-portal-test.sh` (headless; **P4 landed**): starts the
  compositor (`SDE_INPUT_CAPTURE=1`) and the `xdg-desktop-portal-serenity` backend
  on a private session bus, then drives the D-Bus impl interface end to end —
  `SupportedCapabilities`/`version`, `CreateSession2`, `Start` (with the
  `AccessDialog` prompt), `GetZones`, `SetPointerBarriers` (diagonal rejected),
  `ConnectToEIS`, `Enable`, the `Activated` signal after a scripted barrier
  crossing, and `Disable`.
- `tools/input-capture-test.c` (new): a small libei passive client, mirroring
  `tools/screencast-consumer.c` for the capture side; reports
  `device_start_emulating seq=N`, received motion/keys, and can inject emulated
  events.

---

## 9. Out of scope

- **Clipboard capture** (`clipboard_enabled` / `org.freedesktop.portal.Clipboard`)
  — return `false`; a later increment.
- **Touchscreen** capture — no touch support ([`08`](08-compositor-gaps.md) §4).
- **`ext-transient-seat-v1` / multi-seat** capture ([`12`](12-r4.3-portable-session-integration.md) §8).
- X11/`XTest` input capture (no XWayland yet).

---

## 10. Open decisions

| # | Decision | Leaning |
|---|---|---|
| I1 | EIS host | Compositor (Option A, §3) |
| I2 | Capabilities advertised | `POINTER \| KEYBOARD`; touch later |
| I3 | Global gating | `SDE_INPUT_CAPTURE=1` + portal prompt |
| I4 | Compositor release escape hatch | Reserved keybinding (e.g. `Super+Escape`) while active |
| I5 | Persistence | Reuse the ScreenCast restore-token store |

---

## 11. Risks

- **Privilege** — the whole feature is high-risk; §6 is a prerequisite, not
  polish.
- **libei availability/version drift** — `libei 1.6.0` is on Arch; pin a minimum
  and degrade to the `gtk` fallback when absent (the compositor builds
  `libeis`-optional so a no-libei build still works).
- **Activation correctness** — the EIS `sequence == activation_id` ordering and
  `start_emulating`-before-first-event are easy to get subtly wrong; the P2 exit
  test asserts the exact sequence.
- **Never lose the pointer** — a capture session that never releases would freeze
  input; the escape hatch (§6) and thorough `Disable`/`Release` handling are
  mandatory.

---

## 12. References

- Portal specs: `org.freedesktop.portal.InputCapture` and
  `org.freedesktop.impl.portal.InputCapture` v2 (installed under
  `/usr/share/dbus-1/interfaces/`).
- libei/EIS: `libei-1.0`, `libeis-1.0`, `liboeffis` (<https://gitlab.freedesktop.org/libinput/libei>).
- Upstream precedent: KWin and Mutter implement the InputCapture impl portal
  directly and are the EIS implementation for the session.
- [`15-xdg-desktop-portal-serenity.md`](15-xdg-desktop-portal-serenity.md) §5–6,
  [`08-compositor-gaps.md`](08-compositor-gaps.md) §3–4,
  [`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §R5.
