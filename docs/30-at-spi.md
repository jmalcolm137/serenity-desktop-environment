# AT-SPI: exposing the accessibility tree

**Status:** design · **Date:** 2026-10-15

The last and largest item in [`29-accessibility.md`](29-accessibility.md). Assistive
technology (a screen reader, `accerciser`, a speech-to-text tool) talks to
applications through AT-SPI over D-Bus. Today nothing here publishes anything, so
there is no accessibility tree to read: an AT can see the compositor's windows (they
are just surfaces) and nothing inside them.

This is the design; the code should follow it once the shape is agreed.

## What a consumer does

1. It connects to the *accessibility bus*, which the `at-spi-bus-launcher` sets up:
   a private `dbus-daemon` whose address is published on the session bus at
   `org.a11y.Bus.GetAddress` (and in `$AT_SPI_BUS_ADDRESS`).
2. `org.a11y.atspi.Registry` (an `at-spi2-registryd`) is running on that bus. The
   consumer asks it for events (`RegisterEvent`) and for the list of applications.
3. Each application exposes an object tree under a well-known bus name. Every node
   implements `org.a11y.atspi.Accessible` (`GetRole`, `GetName`, `GetChildCount`,
   `GetChildren`, `GetState`, `GetParent`, `GetInterfaces`) and, depending on the
   node, interfaces like `Component` (screen extents, hit-testing), `Action`
   (`GetActions`, `DoAction`), `Text`, `Value`, `Selection`, `EditableText`.
4. Changes arrive as `org.a11y.atspi.Event.*` signals on `org.a11y.atspi.Event.Object`
   and friends: `StateChanged`, `NameChanged`, `TextChanged`, `ValueChanged`,
   `VisibleDataChanged`, …

The parts a *client library* needs to feel native: an application object with a
`/org/a11y/atspi/accessible/root` returning the application node, and stable
`(so)`-style accessible references (bus name, object path) for children.

## Where it belongs

In **LibGUI**, not the compositor. The tree *is* the widget tree: `GUI::Widget`
already knows its children, its geometry, whether it is visible/enabled/focused, and
its `Core::Object` class name. The compositor knows only surfaces, and a screen
reader needs to read a text field's contents, not a window's pixels.

So:

* **`GUI::AccessibilityServer`**: one per application, created lazily the first time
  a widget is registered and only if an accessibility bus is present. It owns the
  root object, mirrors the widget tree, and answers queries.
* **A per-widget opt-in.** Widgets that mean something to an AT publish a role and a
  name; widgets that are pure decoration do not. `GUI::Widget` gets
  `virtual Optional<AccessibilityRole> accessibility_role() const` (default: none)
  and `virtual String accessibility_name() const` (default: the tooltip, else the
  text). A widget with no role is transparent: its children are spliced into its
  parent's child list, so containers do not add noise.
* **No bus, no cost.** If the accessibility bus is not there, the server never
  starts and nothing is mirrored. Applications must not fail, slow down, or log
  because a screen reader is absent; see "Failure is normal" below.

The transport is the D-Bus we already speak (`dbus-1` through AK/LibCore). The
object model is ours.

## The mapping

Roles, from the widget classes the SDE actually uses:

| LibGUI | AT-SPI role |
| --- | --- |
| `GUI::Button` | push button (or toggle button with `is_checkable()`) |
| `GUI::CheckBox` | check box |
| `GUI::RadioButton` | radio button |
| `GUI::Label` | label |
| `GUI::TextEditor` | text (`Text`/`EditableText` when editable) |
| `GUI::TextBox`, `GUI::PasswordBox` | entry |
| `GUI::ComboBox` | combo box |
| `GUI::Slider` | slider (`Value`) |
| `GUI::ScrollBar` | scroll bar (`Value`) |
| `GUI::ListView`, `GUI::TreeView`, `GUI::TableView` | list / tree table / table |
| `GUI::TabWidget` | page tab list |
| `GUI::Menu` | menu; `GUI::MenuItem` → menu item |
| `GUI::Dialog` / `GUI::Window` | dialog / frame |
| `GUI::GroupBox` | panel |
| `GUI::ProgressBar` | progress bar |

States map from what the widget already answers: `is_enabled()` → `ENABLED` and
`SENSITIVE`, `is_visible()` → `VISIBLE`/`SHOWING`, `is_focused()` → `FOCUSED`,
`is_focusable()` → `FOCUSABLE`, plus `CHECKED` for a checkable button,
`EDITABLE` for an editable text widget, `SELECTED` for a selected row.

Geometry comes from `Widget::screen_relative_rect()`, which the compositor already
maintains for the app's own widgets.

## Order of work

1. **Read-only tree.** `AccessibilityServer`, the application node, roles, names,
   geometry, states, and the bus registration. Enough for `accerciser` to show a
   window's widgets and for a screen reader to read a label or a button.
2. **Events.** `StateChanged` (focus in particular: that is what a screen reader
   follows), `NameChanged`, `TextChanged`, `ValueChanged`, emitted from the same
   places the widgets already redraw.
3. **Actions.** `Action::DoAction` for a button's `on_click`, a menu item's
   `on_activate`, a check box's toggle, so a switch/voice tool can *drive* the UI,
   not only read it.
4. **Text.** `Text`/`EditableText` on `TextEditor` and `TextBox`: character counts,
   ranges, `SetTextContents`, caret offsets. This is what a screen reader needs to
   read a Terminal or an editor, and it is the fiddliest part.

Steps 1–2 are the useful subset and should be one change. 3–4 can follow.

## Failure is normal

Four rules, because an accessibility server must never be a reason an application
misbehaves:

* No bus → no server, no mirroring, no log spam.
* A D-Bus error on a query is returned as an AT-SPI error, never asserted on: a
  screen reader disconnecting mid-query must not take the application down.
* The tree is mirrored lazily and bounded; a `ListView` with 100k rows must not
  produce 100k nodes unless the AT asks for them.
* Nothing here may block the GUI thread. Queries are answered on the D-Bus thread
  from a snapshot the GUI thread publishes.

## Testing

Headless, on the same terms as the rest of R5:

* The CI image already installs `dbus`; a test can run `dbus-run-session` plus a
  private `at-spi-bus-launcher` (or a stand-in registry) exactly as
  `run-screencast-test.sh` now brings its own PipeWire.
* A small `tools/atspi-probe.cpp` client walks the tree of a running SDE
  application (a `TextEditor` with a known title, a `Button` with a known label) and
  asserts roles, names, states and geometry; the same client drives step 3 once
  actions exist.
* The negative case matters as much: with no bus, the application must behave
  identically (asserted by the existing app tests running unaffected).

## Open questions

* **Which bus name per application.** AT-SPI's convention is one name per
  application, usually derived from the process. The registry needs to find it; the
  documented path (`org.a11y.atspi.Registry.GetChildren` from the session bus) is the
  least surprising choice and is what `accerciser` uses.
* **Toolkit-generic or Serenity-only roles.** Roles like `PAGE_TAB_LIST` map
  cleanly; the Serenity-specific widgets (a `GUI::Icon` button, the applet area) do
  not. Map to the closest AT-SPI role rather than inventing one.
* **Where the accessibility bus address comes from when there is no launcher.** A
  session without `at-spi-bus-launcher` simply has no accessibility; we should not
  start one ourselves.
