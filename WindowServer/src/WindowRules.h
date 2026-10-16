/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGfx/Rect.h>

// The window manager's geometry and placement rules: which tile a window snaps
// to, what rectangle a tile covers, and whether a window is on the current
// workspace. These are pure functions of their arguments — no compositor state,
// no wlroots, no Wayland — so they are unit-testable on their own
// (test/WindowRulesTest.cpp) and the compositor is free to be wrong about how it
// calls them rather than about what they mean.
// SerenityOS WindowTileType: how a window is snapped to the output. It stays in
// the global namespace (the compositor uses it unqualified throughout); only the
// rules below are namespaced.
enum class WindowTileType {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    VerticallyMaximized,
    HorizontallyMaximized,
    Maximized,
};

namespace WindowServer {

// The geometry a tile type covers within the usable area. Splits use floor
// division, so the first half is never larger than the second and the halves
// always add up to the area (a 101px axis tiles as 50 and 51, never 50 and 50
// with a lost column). The whole area for Maximized (and for None, which is not
// a tile).
Gfx::IntRect tile_rect(Gfx::IntRect const& usable_area, WindowTileType type);

// The tile a drag at (x, y) is aiming for: the edge bands of `usable_area`, their
// corners, or None in the middle. `edge_threshold` is the band's width.
WindowTileType tile_target_for_cursor(Gfx::IntRect const& usable_area, double x, double y, int edge_threshold);

// Whether a window on the given workspace is shown while the given workspace is
// current. Override-redirect windows (menus, tooltips) are transient and are
// never workspace-hidden.
bool window_visible_on_workspace(u32 window_row, u32 window_column, u32 current_row, u32 current_column, bool mapped, bool override_redirect = false);

// --- the window list (the Alt-Tab switcher) ---------------------------------

// What the switcher needs to know about one window. The fields exist precisely
// because the answer differs between an xdg and an X11 window: an X11 window is
// listed only when it is mapped and has a Taskbar entry, while an xdg window is
// listed whenever it exists as a toplevel.
struct SwitcherWindow {
    u32 workspace_row { 0 };
    u32 workspace_column { 0 };
    bool is_x11 { false };
    bool mapped { true };
    bool override_redirect { false };
    bool has_taskbar_entry { true }; // An X11 window without a wm_id has none.
};

// Whether the switcher lists a window: the current workspace only, or every
// stack. Override-redirect windows (menus, tooltips) are never listed — they are
// transient, not windows the user switches to.
bool switcher_lists_window(SwitcherWindow const& window, bool current_workspace_only, u32 current_row, u32 current_column);

// Whether a listed set spans more than one stack (the switcher's multi-stack
// indicator). An empty or single-stack list does not.
bool switcher_spans_stacks(Vector<SwitcherWindow> const& windows);

}
