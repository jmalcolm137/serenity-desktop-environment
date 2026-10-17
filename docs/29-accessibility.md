# Accessibility

**Status:** plan · sticky keys and high contrast implemented · **Date:** 2026-10-15

`docs/04-known-gaps.md` lists four things still absent under R5: **AT-SPI**, **sticky
keys**, **magnifier** and **high contrast**. This is the plan for them: what each
actually needs, what already exists to build on, and the order to do them in.

Accessibility here means the desktop-level pieces, not (only) the applications':
the compositor owns input, the theme and the screen, so it is the compositor that
has to provide the assistive input behaviour and the high-contrast mode, and it is
the compositor plus LibGUI that have to expose the accessibility tree.

## What already exists

* **A high-contrast palette.** The framework ships `Base/res/themes/Contrast.ini`,
  selectable in Display Settings → Themes. It is a theme, though, not an
  accessibility mode: nothing keeps it on, nothing forces applications to it, and
  the window frame is drawn from whatever theme happens to be selected.
* **A magnifier application.** SerenityOS's `Magnifier` is built (`docs/04`,
  Demos/Utilities; `scripts/run-demos-test.sh`). It captures the screen itself.
* **A theme broker.** Changing the theme in Display Settings publishes it to
  `/run/user/<uid>/sde-theme` and the port applies it to every client
  (`WindowServerConnection::apply_theme_from_broker`), so one place can change the
  whole desktop's palette at runtime.
* **Input routing to test.** `tools/virtual-input-test.c` injects key and pointer
  events through the wlroots virtual-input protocols and reads back what a client
  received (`scripts/run-terminal-keys-test.sh`), so an assistive *input* feature
  can be asserted end to end and headless.

## 1. Sticky keys

**Done.** `[Accessibility] StickyKeys` (default off) in `WindowServer.ini`.

A modifier pressed and released on its own latches: the compositor keeps it active
so the next key press is delivered with it, then clears it. Shift → `a` produces
`A`; Ctrl → `c` is delivered as Ctrl-C. Pressing two modifiers holds both (Shift
then Ctrl → Shift+Ctrl for the next key), and pressing the same modifier twice in a
row cancels the latch, so a user who latches by accident is not stuck.

The latch is applied where the compositor already computes the modifier masks, so
everything downstream sees it: xkb symbol lookup (which is what makes `a` come out
as `A`), the `wl_keyboard.modifiers` a client receives, keybindings, and
InputCapture. Guarded by `scripts/run-sticky-keys-test.sh`.

## 2. High contrast

**Done.** `[Accessibility] HighContrast` (default off).

When it is on the compositor applies the `Contrast` palette to its own chrome *and*
publishes it to the theme broker, so every client follows; and a theme change
through the broker does not turn the mode off; the compositor says so in its log
and puts the broker back to what is actually applied (only when they differ, so
this cannot loop). Guarded by `scripts/run-high-contrast-test.sh`, which asserts the
control (off is inert) as well.

## 3. Screen magnifier

**Not started; the blocker is measured, and one earlier claim was wrong.** The
application exists, is built, and *works*: run headless it maps and presents a
248x248 toplevel (`toplevel mapped title='Magnifier' app_id='Magnifier'`) and keeps
painting. What it shows is empty, because it captures the screen through
`GUI::ConnectionToWindowServer::the().get_screen_bitmap_around_cursor()` /
`…_around_location()`, and on the port both resolve to the default stub in
`WindowServerDefaultStub.h`, which returns a null bitmap — the compositor serves
neither. So the work is to serve those two methods, not to write a magnifier.

(An earlier note here said it "never maps". That was wrong — it was measured with the
window check looking at the wrong log.)

**The wire format already exists.** `serenity_window_manager` already moves raw
bitmaps both ways for the wallpaper: an event/request of
`(width, height, pitch, format, fd)` where `format` is a `wl_shm` value and the fd
carries the pixels. A capture reply is the same shape, so no new encoding.

**Push, not request/reply.** The shim has no synchronous Wayland request: every sync
answer it gives (`get_screen_layout`, `get_wallpaper`, `get_global_cursor_position`)
comes from state the compositor *pushes*. Keep that shape: the shim asks for a region
(`request_capture(serial, x, y, w, h)`), the compositor replies with a
`captured(serial, …)` event, and the shim caches the latest bitmap. `get_screen_bitmap_*`
returns that cache — so the magnifier sees the previous frame, which at one refresh
interval is invisible. Blocking the UI thread on a round-trip would work too, but it
is the one thing this architecture deliberately avoids.

**The rendering, which is the real work.** Nothing in the tree renders the *scene* to
a CPU-readable buffer. `Overlays.cpp`'s `snapshot_toplevel()` only reads one client's
buffer (and gives up on a DMA-BUF). The vendors wlroots 0.20 has the pieces:
`wlr_allocator_autocreate(backend, renderer)` → `wlr_allocator_create_buffer()`, and
`wlr_scene_output_build_state(scene_output, state, options)` where
`options.swapchain` may be a custom `wlr_swapchain_create(...)`. Open question to
settle with a scratch experiment *before* wiring anything: after `build_state`
renders into a custom swapchain, how to get the rendered buffer back for reading
(`build_state` acquires and submits, and does not hand the buffer to the caller), and
whether that path disturbs the output's damage ring — the compositor renders every
frame through `wlr_scene_output_commit`, so a second `build_state` must not steal the
next frame's damage. Both are cheap to test in isolation and expensive to discover in
the frame path.

**Order of work once that is settled:** compositor region capture (+ a headless test
that renders a known scene and checks the region's pixels) → the protocol request and
event → the shim's cache and the two stub overrides → `run-magnifier-test.sh`
(assert the window maps *and* that captures are served, so a blank magnifier fails).

**Still true from the original plan:** the magnifier also has to be usable without
the pointer leaving it, i.e. the region it covers must not steal clicks.

## 4. AT-SPI

**Not started; the large one.** An assistive technology (a screen reader, a
speech-to-text tool, `accerciser`) talks to applications over D-Bus: it enumerates
an accessibility tree of objects (application → window → widget, each with role,
name, state, actions) and asks for events. Doing this here means:

* **LibGUI publishes the tree.** Widgets already have enough structure (the widget
  tree, roles/tooltips/actions), but nothing exports it. An `AccessibilityServer`
  in LibGUI would mirror the widget tree into the AT-SPI object model and emit
  events (focus, value change, text change) as widgets change.
* **A bridge to the bus.** AT-SPI's transport is `dbus-daemon` plus
  `at-spi-bus-launcher`; the ATK/AT-SPI2 adapter expects `org.a11y.Bus` and the
  `org.a11y.atspi.*` interfaces. The SDE already speaks `dbus-1` through
  AK/LibCore, so the work is in the object model, not the transport.
* **Only then is it testable**, but it *is* testable headlessly: an
  `org.a11y.atspi.Accessible` query against a running Terminal or TextEditor, driven
  by a small D-Bus client in `tools/`, needs no display input.

Designed in [`30-at-spi.md`](30-at-spi.md): LibGUI publishes the tree (the tree *is*
the widget tree), the transport is the `dbus-1` we already speak, and it lands in
four steps: read-only tree, then events, then actions, then the text interfaces. It
is an order of magnitude more work than 1–3 combined.

## Order

1. Sticky keys: done.
2. High contrast: done.
3. Magnifier: needs the capture path wired, but the application exists.
4. AT-SPI: design doc first, then LibGUI's tree and the bridge.

## Testing notes

Everything here is assertable headless, which is the constraint the rest of R5
already works under: input features through `tools/virtual-input-test.c`, theme and
mode features by reading the broker file and the compositor's own logs, and AT-SPI
over D-Bus. None of them needs a real display, a session, or an assistive
technology installed.
