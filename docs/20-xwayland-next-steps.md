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
| `07027fc` | **Alt-Tab** (and the `WindowRef` abstraction). `WindowRef` (an xdg `Toplevel` **or** an `X11Toplevel`, `Model.h`) is the small window abstraction the WM policy paths share. `WindowSwitcher` lists X11 windows alongside xdg ones: title, live thumbnail, frame rect, most-recently-focused order (X11 gained `focus_order`), select → focus, and the theme's *Highlighted* frame state. X11 windows list on the current stack. |
| `0cf1933` | **Workspaces**. `X11Toplevel` gained `workspace_row`/`workspace_column`; a mapped X11 window joins the current stack and hides/shows with it (workspace switch and grid reload), and the switcher filters/labels X11 windows by stack. |
| `b3ac13a` | **Icons + WM_CLASS**. Fetch `_NET_WM_ICON` (`wlr_xwayland_surface_fetch_icon` + `xcb_ewmh`) into a `Gfx::Bitmap` and report it (`emit_window_icon`) so the Taskbar button and the switcher draw it; read `WM_CLASS` (`instance`/`class`) onto the toplevel. |
| `cb21165` | **Override-redirect polish**. Menus/tooltips stay visible across workspace switches (never workspace-hid) and are re-raised above their parent when it is focused/raised. |

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

## The window abstraction

The WM internals are typed on the xdg **`Toplevel`**: `WindowSwitcher`,
`WindowManager::update_toplevel_visibility` / `emit_window_icon` /
`find_toplevel_by_wm_id`, the frame hit-test/interactive paths, and the workspace
grid. X11 windows join those through a small **window abstraction**, `WindowRef`
(`Model.h`; landed in `07027fc`):

```cpp
// A window is either an xdg toplevel or an X11 (Xwayland) window.
struct WindowRef {
    Toplevel* xdg { nullptr };
    X11Toplevel* x11 { nullptr };
    bool is_x11() const { return x11 != nullptr; }
};
```

The `WindowSwitcher` now goes through it (its `title`/`rect`/`thumbnail`/`focus`/
`minimize` accessors live beside its use), which landed item 1. Items 2–3 reuse
the same type; their accessors (`rect`/`focus`/`set_minimized` for the workspace
grid and icon paths) hoist into shared helpers as those land.

## Remaining for parity (in a suggested order)

1. ~~**Alt-Tab**~~ — **done** (`07027fc`): X11 windows join the `WindowSwitcher`
   list (title, live thumbnail, select → focus; the *Highlighted* frame state).
2. ~~**Workspaces**~~ — **done** (`0cf1933`): an X11 window is assigned to the
   current stack on map and hides/shows with it (`update_x11_toplevel_visibility`).
3. **Window icons** — **done** (`b3ac13a`): `_NET_WM_ICON` is fetched
   (`wlr_xwayland_surface_fetch_icon` + `xcb_ewmh`) and emitted
   (`emit_window_icon`). **WM_CLASS grouping** is read onto the toplevel
   (`wm_class`/`wm_instance`) but the Taskbar still keys buttons per
   `WindowIdentifier{client_id, window_id}` (one button per window) and has no
   app-grouping mode, so grouping needs a Taskbar-side change (in the port).
4. ~~**Override-redirect polish**~~ — **done** (`cb21165`): menus/tooltips are
   never workspace-hid and stay above their parent.
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
