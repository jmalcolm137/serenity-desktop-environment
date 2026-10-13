# R5 — Xwayland: current state & next steps

**Status:** in progress · **Updated:** 2026-10-13

This is the resume point for the XWayland work (R5, "Compatibility &
ecosystem"). The goal is parity with **Plasma 6 / KWin**'s Xwayland support: X11
clients are first-class windows in the Serenity Desktop Environment.

Compositor-facing background lives in [`04-known-gaps.md`](04-known-gaps.md) and
[`08-compositor-gaps.md`](08-compositor-gaps.md). This doc records what is done
and exactly what remains.

## What is done

Xwayland runs **rootless** (wlroots spawns `Xwayland -rootless -wm`), so every
X11 top-level is its own `wlr_xwayland_surface` → its own Wayland surface, which
the compositor maps as a separate scene surface. Landed, in order:

| Commit | Scope |
|---|---|
| `133f97e` | **Display**: X11 windows map, position, click-to-focus, stack, close; `DISPLAY` exported (state file `xwayland-display` + the compositor env); `[Compositor] XWayland` (`WindowServer.ini`, default true) starts no X server when `false`. Input hit-testing covers X11 surfaces (`window_surface_at` / `focus_window_at`). |
| `ae5a602` | **Server-side decoration frames**: the SerenityOS frame (titlebar + close/maximize/minimize buttons) around X11 windows unless the client asks for none (Motif hints) or is override-redirect; titlebar drag-to-move, border resize, Super+`<button>` move/resize. |
| `44ce17b` | **Taskbar window list**: X11 windows are reported to the WindowManagerServer by `wm_id` (map/unmap/title/focus/destroy) and the Taskbar's activate/minimize requests route back (host hooks `focus_x11` / `set_minimized_x11`). |
| `8bba52e` | **Override-redirect**: menus/tooltips never take keyboard focus. |

Where the code lives:

- `WindowServer/src/main.cpp` — the X11 model wiring: `server_new_xwayland_surface`,
  the `x11_surface_*` listeners, `focus_x11_toplevel`, `x11_update_frame` /
  `x11_hit_test_frame`, `x11_begin_interactive`, `handle_x11_frame_*`,
  `focus_window_at`, and the `hook_*_x11` calls into the WM.
- `WindowServer/src/Model.h` — `struct X11Toplevel`.
- `WindowServer/src/WindowManager.{h,cpp}` — `emit_window_state(X11Toplevel&)`,
  `find_x11_by_wm_id`, and the Taskbar request routing.
- `scripts/run-xwayland-test.sh` — X11 client maps, gets a frame, and is reported
  to the WM (both with XWayland on and off).

Note: wlroots' `xwayland.h` declares a C field named `class`, a C++ keyword, so
`main.cpp` includes it with a scoped `#define class class_` (the field is never
read).

## The blocker for the rest

The WM internals are typed on the xdg **`Toplevel`**: `WindowSwitcher`,
`WindowManager::update_toplevel_visibility` / `emit_window_icon` /
`find_toplevel_by_wm_id`, the frame hit-test/interactive paths, and the workspace
grid. X11 windows can only join those once there is a small **window
abstraction**. Suggested shape:

```cpp
// A window is either an xdg toplevel or an X11 (Xwayland) window.
struct WindowRef {
    Toplevel* xdg { nullptr };
    X11Toplevel* x11 { nullptr };
    bool is_x11() const { return x11 != nullptr; }
};
```

with helpers `title(WindowRef)`, `rect(WindowRef)`, `focus(WindowRef)`,
`set_minimized(WindowRef, bool)`. That unlocks items 1–3 below together.

## Remaining for parity (in a suggested order)

1. **Alt-Tab** — include X11 windows in the `WindowSwitcher` list (title,
   rect/thumbnail, select → focus). Needs the `WindowRef` refactor; the switcher's
   highlight uses the theme's *Highlighted* state, which `x11_update_frame` does
   not yet render.
2. **Workspaces** — assign each X11 window to the current stack grid cell and
   apply `update_toplevel_visibility` on switch (X11 windows are currently always
   visible, on every workspace).
3. **Window icons + WM_CLASS** — fetch `_NET_WM_ICON` (`wlr_xwayland_surface_fetch_icon`
   + `xcb_ewmh_get_wm_icon_iterator`) and emit it (`emit_window_icon`), and group
   Taskbar buttons by `WM_CLASS`/`instance`.
4. **Override-redirect polish** — position/raise menus & tooltips like ordinary
   unmanaged windows (they are mapped but not tuned for stacking).
5. **Selection / clipboard bridge** — X11 `PRIMARY`/`CLIPBOARD` ↔ the Wayland data
   device, so copy/paste works between X11 and native apps (wlroots' XWM owns the
   X side; the compositor must bridge it).
6. **Xwayland keyboard grabs** — honour X11 keyboard grabs (wlroots
   `xwayland-keyboard-grab`), so X11 menus/screensavers that grab the keyboard work.
7. **`_NET_WM_STATE` hints** — modal, skip-taskbar, above, demands-attention; and
   **transients** (parent/child): children stay with their parent, transient dialogs
   center on it.
8. **`_NET_WM_STRUT` workarea** — reserve space for X11 panels/docks and report the
   usable area (`wlr_xwayland_set_workareas`).
9. **X11 ↔ Wayland drag-and-drop** — the XDND ↔ `wl_data_device` bridge, the X11
   counterpart of the compositor DnD already shipped.
10. **Focus-stealing prevention / `_NET_ACTIVE_WINDOW`** — proper attention policy
    for X11 clients requesting activation.

## Testing notes

The headless harness drives X11 with `xterm`/`xmessage` (present on the dev box).
`scripts/run-xwayland-test.sh` is the home for new assertions (e.g. icons, Alt-Tab,
workspaces). Anything that needs a compositor with a Taskbar can run one as in
`scripts/run-headless-test.sh`; `scripts/run-virtual-touch-test.sh` +
`tools/virtual-input-test.c` drive pointer/keyboard.
