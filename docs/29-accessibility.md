# Accessibility

**Status:** plan · sticky keys and high contrast implemented · **Date:** 2026-10-15

`docs/04-known-gaps.md` lists four things still absent under R5: **AT-SPI**, **sticky
keys**, **magnifier** and **high contrast**. This is the plan for them — what each
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
through the broker does not turn the mode off — the compositor says so in its log
and puts the broker back to what is actually applied (only when they differ, so
this cannot loop). Guarded by `scripts/run-high-contrast-test.sh`, which asserts the
control (off is inert) as well.

## 3. Screen magnifier

**Not started.** The application exists; what it needs on Wayland is a screen
source. Two options, in order of preference:

1. Wire `Magnifier` to the SDE's own `ext-image-copy-capture-v1` path (the one
   ScreenCast and the screenshot tools already use), so it magnifies the real
   output without a portal round-trip. This is the compositor-native answer and
   lets the magnifier follow the focused output.
2. Fall back to the ScreenCast portal (`docs/15`) if the application's capture
   abstraction cannot be pointed at an in-process source.

The magnifier also has to be usable without the pointer leaving it, i.e. the input
region it covers must not steal clicks; the compositor can keep a "magnifier"
layer surface input-transparent.

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

This is the item that should get its own design doc (`docs/30-at-spi.md`) before
code lands; it is an order of magnitude more work than 1–3 combined.

## Order

1. Sticky keys — done.
2. High contrast — done.
3. Magnifier — needs the capture path wired, but the application exists.
4. AT-SPI — design doc first, then LibGUI's tree and the bridge.

## Testing notes

Everything here is assertable headless, which is the constraint the rest of R5
already works under: input features through `tools/virtual-input-test.c`, theme and
mode features by reading the broker file and the compositor's own logs, and AT-SPI
over D-Bus. None of them needs a real display, a session, or an assistive
technology installed.
