/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

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

}
