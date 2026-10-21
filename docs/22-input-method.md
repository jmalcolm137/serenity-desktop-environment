# R5: Text input / IME (`wp_text_input_v3` + `zwp_input_method_v2`)

**Status:** done · **Updated:** 2026-10-13
**Related:** [`04-known-gaps.md`](04-known-gaps.md) (R5) · [`08-compositor-gaps.md`](08-compositor-gaps.md) §4 ·
[`11-r4-implementation-plan.md`](11-r4-implementation-plan.md) §11 (protocol table) · [`21-r5-protocol-parity-plan.md`](21-r5-protocol-parity-plan.md)

This records the compositor-side IME support: the standard **text-input-v3**
(`zwp_text_input_v3`, used by clients' text fields) and **input-method-v2**
(`zwp_input_method_v2`, used by an input method such as an on-screen keyboard or
a CJK engine front end). Both are unstable Wayland protocols and both are
implemented by the pinned wlroots 0.20; the compositor supplies the glue between
them.

## 1. Model

The compositor is the **relay**: a client text field and an input method never
talk to each other directly.

```
 client text field ──wp_text_input_v3──▶ compositor ──zwp_input_method_v2──▶ input method
      (LibGUI / toolkit)                  relay                            (maliit / fcitx5-wayland / …)
```

For each client `zwp_text_input_v3` the compositor keeps a `TextInput` relay
(`Model.h`) and for each input method an `InputMethodClient`; candidate windows
(`zwp_input_popup_surface_v2`) are `InputPopup`s. The state machine follows
sway's `sway/input/text_input.c` (the reference compositor implementation):

- **Focus.** On every keyboard-focus change the compositor sends `enter` to the
  text inputs whose client owns the focused surface (`input-method` is per
  surface) and `leave` to the others (`InputMethod::set_focus`, called from
  `focus_toplevel` / `focus_x11_toplevel` / `focus_layer_surface` /
  `restore_keyboard_focus` / the session lock). If no input method is connected
  yet the surface is remembered as *pending* and `enter` is sent when one
  connects, so an input method started after a field is focused still works.
- **Activation.** When the field is enabled (`events.enable`) or commits while
  enabled, the compositor sends the input method `activate` + the field state
  (`surrounding_text` / `text_change_cause` / `content_type`, honouring the
  features the client advertised) + `done`. Leaving a field (or disabling it)
  sends `deactivate`.
- **Composing.** The input method's `commit` is applied back to the field:
  `delete_surrounding_text`, then `preedit_string`, then `commit_string`, then
  `done` (the order the protocol specifies). The serial is the input method's
  own double-buffering serial, handled by wlroots.
- **Keyboard grab.** An input method may take the keyboard
  (`zwp_input_method_v2.grab_keyboard`). While a grab is held, key and modifier
  events are forwarded to it (via `InputMethod::handle_key/handle_modifiers`,
  hooked into `keyboard_key` / `keyboard_modifiers`), after the compositor's own
  keybindings. A release is only forwarded when its press was; on grab release
  the modifiers are re-sent to the focused client. Keys are **not** replayed to
  the client while grabbed.
- **Candidate popups.** `get_input_popup_surface` surfaces are rendered in a
  scene tree above the shell (`Server::ime_popup_tree`), positioned just below
  the field's caret (`cursor_rectangle`), clamped to the output, and follow the
  focused surface (hidden when nothing is focused). Each commit re-constrains
  them, and `text_input_rectangle` is sent so the input method can lay out the
  candidate list.
- **One input method per seat.** A second `get_input_method` on the same seat is
  answered with `unavailable`. If the input method disconnects while a field is
  focused, the field is left (kept *pending* for a future input method) and any
  grab is torn down.

## 2. Code

| Where | What |
|---|---|
| `Services/WindowServer/src/InputMethod.{h,cpp}` | the whole relay: managers, focus, activation, apply-back, keyboard grab, candidate popups. |
| `Services/WindowServer/src/Model.h`, `ImeState.h` | `TextInput`, `InputMethodClient`, `InputPopup` (`Model.h`); `ImeState` (`ImeState.h`), held as `Server::ime` (`text_input_manager`, `input_method_manager`, `text_inputs`, `input_methods`, `input_popups`, `input_method`, `popup_tree`). |
| `Services/WindowServer/src/main.cpp`, `Focus.cpp`, `Keyboard.cpp` | `InputMethod::initialize(server)` in `initialize()`; `InputMethod::set_focus` at the focus sites (`Focus.cpp`, `XWayland.cpp`, `LayerShell.cpp`); `InputMethod::handle_key` / `handle_modifiers` in the keyboard path (`Keyboard.cpp`). |
| `Services/WindowServer/src/wlr_input_method_v2_cxx.h` | shim: `<wlr/types/wlr_input_method_v2.h>` has a struct member literally named `delete`, so it cannot be compiled as C++; rename it for the include (only place the header is pulled in). |

## 3. Testing

`scripts/run-ime-test.sh` (headless, `set -euo pipefail`) builds two Wayland
clients and drives the whole pipeline:

- `Tools/text-input-test.c`: an xdg-shell client with a `wp_text_input_v3`
  field; prints `enter` / `preedit` / `commit` / `done`.
- `Tools/input-method-test.c`: a `zwp_input_method_v2` client; on activation
  sends a preedit and a commit, requests the keyboard grab, draws a candidate
  popup, and prints everything it observes.

It asserts both globals are advertised, the field received `enter`, preedit and
commit, the input method received `activate` and the surrounding text, the popup
was positioned at the caret, an injected key reached the input method (not the
client), and the compositor survived both clients exiting (grab teardown).
`scripts/run-protocol-matrix-test.sh` also checks the two globals.

**Manual check (a real input method).** A genuine on-screen/CJK input method is
worth trying by hand: `maliit-keyboard`/`maliit-server`, `squeekboard`, or
`fcitx5`'s Wayland front end speak `zwp_input_method_v2` (note: `ibus-daemon`
does **not**; it uses its own D-Bus protocol). Start one inside the session and
type into any `text-input-v3` client (GTK 4 / Qt 6 apps, the SDE Terminal).
Not covered by CI, since none of those clients ship on the dev box.

## 4. Not done

- **`ext-transient-seat-v1`** for on-screen keyboards that want their own seat /
  surface routing; the plain `zwp_input_method_v2` path above does not need it.
- **Text-input-v3 v2 features** (`action`, `language`, `preedit_hint`); the
  compositor binds v1; the extra events are left to a later pass.
- Accessibility (AT-SPI) remains out of scope here (see
  [`08-compositor-gaps.md`](08-compositor-gaps.md) §4).
