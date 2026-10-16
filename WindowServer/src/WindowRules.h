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

// --- the window-stack overlay and transition (R3) ---------------------------

// The metrics of the stack-switch overlay: a grid of one cell per stack, padded
// and inset from the usable area.
struct StackOverlayMetrics {
    int cell_width { 40 };
    int cell_height { 30 };
    int padding { 8 };
    int margin { 16 };
};

// The overlay's frame, centred on the usable area: the grid plus its padding, and
// the margin around that.
Gfx::IntRect stack_overlay_frame(Gfx::IntRect const& usable_area, int rows, int columns, StackOverlayMetrics const& metrics = {});

// The rect of one cell of the grid, in the overlay's own coordinate space (what
// the painter draws in: 0,0 is the frame's top-left corner, so add the frame's
// location to place it on screen).
Gfx::IntRect stack_overlay_cell(int rows, int columns, int row, int column, StackOverlayMetrics const& metrics = {});

// How far the outgoing and incoming stacks are offset while switching from one
// stack to another, so the incoming stack slides in from the side it came from
// (SerenityOS's convention: moving right slides the old stack left). {0,0} when
// the stack did not change, which is the caller's cue that there is nothing to
// animate.
Gfx::IntPoint stack_transition_delta(u32 new_row, u32 new_column, u32 old_row, u32 old_column, Gfx::IntSize area);

// The transition's eased progress for a linear 0..1 time, and the integer
// position that progress puts a coordinate (the rounding is part of the motion:
// it is what makes the step discrete rather than accumulating float error).
float stack_transition_eased(float t);
int stack_transition_lerp(int from, int to, float eased);

}
