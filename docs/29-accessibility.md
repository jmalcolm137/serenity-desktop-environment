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

**Working.** The application was already there and already mapping a 248x248
toplevel; two stubs in the port kept it empty.

**It was blank for two reasons, not one.** The obvious one: it captures the screen
through `GUI::ConnectionToWindowServer::the().get_screen_bitmap_around_cursor()` /
`…_around_location()`, and both resolved to the null bitmap in
`WindowServerDefaultStub.h`. The one that hid behind it: the magnifier captures from a
`GUI::DisplayLink` callback (its capture loop *is* the display link), and
`enable_display_link` / `disable_display_link` were also empty stubs — so `sync()` was
never called at all, and serving the bitmap alone would still have shown nothing.
Both are implemented now; the second is why the test waits for *several* captures
rather than one.

**The wire format already existed.** `serenity_window_manager` already moved raw
bitmaps both ways for the wallpaper: `(width, height, pitch, format, fd)` where
`format` is a `wl_shm` value. A capture is the same shape, so no new encoding.
`request_capture(serial, x, y, w, h)` asks; `captured(serial, w, h, pitch, format, fd)`
answers.

**Push, not request/reply.** The shim has no synchronous Wayland request: every sync
answer it gives comes from state the compositor *pushes*. A capture keeps that shape —
the shim asks for a region and returns the most recent bitmap it was sent, so the
magnifier sees the previous frame. At one refresh interval that is invisible, and
blocking the UI thread on a round trip is the one thing this architecture deliberately
avoids.

**The capture comes from the frame that is on screen.** The first design was to render
the scene offscreen into a swapchain of our own. That does not work here: wlroots does
not export `wlr_swapchain_create`/`acquire` (they are internal to its own capture
implementations), and the visible frame's buffer is a DMA-BUF on a GL/DRM session, so
it cannot simply be mapped. Instead the compositor answers on the next frame it
commits: `output_commit` already fires with the state carrying the presented
`wlr_buffer`, so the request is queued and served there — `wlr_texture_from_buffer` +
`wlr_texture_read_pixels` into a `Gfx::Bitmap`, which works for DMA-BUF and SHM alike.
This reads exactly what the user sees, leaves the visible frame untouched, needs no
swapchain, and costs one refresh interval — which the push model already accepts. The
compositor renders every frame, so a request is never waiting on a repaint that will
not happen; a pending request is dropped if its manager resource goes away.

**The display link is paced by the window surface.** SerenityOS drives
`DisplayLink` from the window server's frame loop. Here the shim arms one
`wl_surface_frame` callback on the most recently presented window while a link is
enabled, and re-arms it after each callback; the callback becomes
`async_display_link_notification()` to the app. A surface the compositor renders
receives `frame_done` every refresh, and the app's repaint in response is the damage
that keeps it coming. The first connection owns the hook, as with host settings, and
its destructor clears it.

**Order of work, as built:** compositor region capture from the committed frame →
the protocol request and event (mirrored in the framework's copy of the XML) → the
shim's cache, the two stub overrides and the display link → `run-magnifier-test.sh`,
which asserts the window maps, that *more than one* capture is served, that the
capture is not empty, and that the app stays up.

**Still true from the original plan:** the magnifier also has to be usable without
the pointer leaving it, i.e. the region it covers must not steal clicks. The pointer
stays where it is; the magnifier's own "lock location" mode is the app's job.

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
