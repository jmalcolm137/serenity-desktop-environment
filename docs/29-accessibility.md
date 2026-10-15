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

**Not started; the blocker is identified.** The application exists and is built, but
it captures the screen through `GUI::ConnectionToWindowServer::the()
.get_screen_bitmap_around_cursor()` / `…_around_location()`, and on the port both
resolve to the *default stub* in `WindowServerDefaultStub.h`, which returns a null
bitmap; the compositor serves neither. Measured: `Magnifier` starts, then sits for
six seconds without ever mapping a window and without an error, which is what a
display-link callback blocking on an IPC that is never answered looks like. So the
work is to serve those two methods rather than to write a magnifier.

Two options for the source, in order of preference:

1. Serve `get_screen_bitmap(rect, screen_index)` / `…_around_cursor` /
   `…_around_location` from the compositor's own capture machinery
   (`ext-image-copy-capture-v1`, the same path the screenshot portal and the
   Taskbar's window thumbnails already use), and forward them from the shim over the
   existing serenity-window connection. That magnifies the real output with no
   portal round-trip, and lets the magnifier follow the focused output. This is the
   smaller change: the capture already exists, only the two methods do not.
2. Fall back to the ScreenCast portal (`docs/15`) if the application's capture
   abstraction cannot be pointed at an in-process source.

The magnifier also has to be usable without the pointer leaving it, i.e. the input
region it covers must not steal clicks; the compositor can keep a "magnifier"
layer surface input-transparent.
**Where the two methods land.** `get_screen_bitmap*` is a WindowServer IPC method, so
serving it is three pieces, and this is the order that keeps each one testable:

1. *Compositor:* a `capture_output_region(server, Gfx::IntRect) -> RefPtr<Gfx::Bitmap>`.
   Nothing in the tree renders the *scene* to a bitmap today: `Overlays.cpp`'s
   `snapshot_toplevel()` reads one client's buffer directly (and gives up on a
   DMA-BUF), which is enough for a window thumbnail and not enough for a screen
   region. The route is wlroots' own render path:
   `wlr_scene_output_build_state()` into a custom buffer, then a readback
   (`wlr_texture_read_pixels`), both present in the vendored 0.20. Assert it
   headless (render a scene, read a region, compare pixels) before wiring anything to
   it.
2. *Framework:* the `Messages::WindowServer::GetScreenBitmap*` IPC definitions and
   the shim's `WindowServerConnection` overrides (it currently inherits
   `WindowServerDefaultStub`, which returns a null bitmap). The
   `…_around_cursor` / `…_around_location` variants are a rect around a point, so the
   shim can derive them from the one region message; the cursor position is the
   compositor's to answer.
3. *Test:* a `run-magnifier-test.sh` in the same shape as the other app tests: start
   Magnifier, assert it maps a window and that the captured bitmap matches a known
   scene region.

That is a real feature, not a wiring job, and it should be its own change.

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
