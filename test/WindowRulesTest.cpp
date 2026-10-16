/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Unit tests for the window manager's pure placement rules (WindowRules.h):
// what rectangle a tile covers, which tile a drag position aims for, and whether
// a window is shown on the current workspace.
//
// These have no compositor, no seat and no wlroots: they are functions of their
// arguments, so a wrong answer is a failed assertion here instead of a window
// that lands one column off on a live session. The integration scripts
// (run-edge-snap-test.sh and friends) then only have to prove that the
// compositor *calls* these with the right arguments.

#include <AK/Format.h>
#include <LibMain/Main.h>

#include "WindowRules.h"

using namespace WindowServer;

namespace {

int g_failures = 0;

void expect(bool condition, char const* what)
{
    if (condition) {
        outln("  ok: {}", what);
    } else {
        outln("  FAIL: {}", what);
        ++g_failures;
    }
}

void expect_rect(Gfx::IntRect const& actual, Gfx::IntRect const& expected, char const* what)
{
    if (actual == expected) {
        outln("  ok: {}", what);
    } else {
        outln("  FAIL: {} (got {},{} {}x{}; want {},{} {}x{})", what,
            actual.x(), actual.y(), actual.width(), actual.height(),
            expected.x(), expected.y(), expected.width(), expected.height());
        ++g_failures;
    }
}

void expect_tile(WindowTileType actual, WindowTileType expected, char const* what)
{
    if (actual == expected) {
        outln("  ok: {}", what);
    } else {
        outln("  FAIL: {} (got {}; want {})", what, to_underlying(actual), to_underlying(expected));
        ++g_failures;
    }
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    outln("=== tile_rect ===");
    // A deliberately odd-sized area: the halves must still cover it exactly.
    Gfx::IntRect const area { 10, 20, 101, 51 };
    int const half_w = 101 / 2; // 50
    int const half_h = 51 / 2;  // 25

    expect_rect(tile_rect(area, WindowTileType::Left), { 10, 20, half_w, 51 }, "Left is the left half, full height");
    expect_rect(tile_rect(area, WindowTileType::Right), { 10 + half_w, 20, 101 - half_w, 51 }, "Right starts at the middle and takes the rest");
    expect_rect(tile_rect(area, WindowTileType::Top), { 10, 20, 101, half_h }, "Top is the top half, full width");
    expect_rect(tile_rect(area, WindowTileType::Bottom), { 10, 20 + half_h, 101, 51 - half_h }, "Bottom starts at the middle and takes the rest");
    expect_rect(tile_rect(area, WindowTileType::TopLeft), { 10, 20, half_w, half_h }, "TopLeft is the top-left quarter");
    expect_rect(tile_rect(area, WindowTileType::TopRight), { 10 + half_w, 20, 101 - half_w, half_h }, "TopRight is the top-right quarter");
    expect_rect(tile_rect(area, WindowTileType::BottomLeft), { 10, 20 + half_h, half_w, 51 - half_h }, "BottomLeft is the bottom-left quarter");
    expect_rect(tile_rect(area, WindowTileType::BottomRight), { 10 + half_w, 20 + half_h, 101 - half_w, 51 - half_h }, "BottomRight is the bottom-right quarter");
    expect_rect(tile_rect(area, WindowTileType::Maximized), area, "Maximized is the whole area");
    expect_rect(tile_rect(area, WindowTileType::None), area, "None is the whole area (it is not a tile)");

    // The property the floor division exists for: the halves tile the area with
    // no lost column or row, whatever the parity.
    for (int width : { 1, 2, 3, 100, 101, 799, 1280 }) {
        Gfx::IntRect const odd { 0, 0, width, 37 };
        auto left = tile_rect(odd, WindowTileType::Left);
        auto right = tile_rect(odd, WindowTileType::Right);
        bool const covers = left.width() + right.width() == width
            && right.x() == left.width()
            && left.width() <= right.width();
        expect(covers, "Left+Right cover the width exactly and the left half is never larger");
    }

    outln("=== tile_target_for_cursor ===");
    Gfx::IntRect const screen { 0, 0, 1280, 720 };
    int const edge = 16;

    expect_tile(tile_target_for_cursor(screen, 640, 360, edge), WindowTileType::None, "the middle aims at no tile");
    expect_tile(tile_target_for_cursor(screen, 5, 360, edge), WindowTileType::Left, "the left band aims Left");
    expect_tile(tile_target_for_cursor(screen, 1275, 360, edge), WindowTileType::Right, "the right band aims Right");
    expect_tile(tile_target_for_cursor(screen, 640, 5, edge), WindowTileType::Top, "the top band aims Top");
    expect_tile(tile_target_for_cursor(screen, 640, 715, edge), WindowTileType::Bottom, "the bottom band aims Bottom");
    expect_tile(tile_target_for_cursor(screen, 5, 5, edge), WindowTileType::TopLeft, "the top-left corner aims TopLeft");
    expect_tile(tile_target_for_cursor(screen, 1275, 5, edge), WindowTileType::TopRight, "the top-right corner aims TopRight");
    expect_tile(tile_target_for_cursor(screen, 5, 715, edge), WindowTileType::BottomLeft, "the bottom-left corner aims BottomLeft");
    expect_tile(tile_target_for_cursor(screen, 1275, 715, edge), WindowTileType::BottomRight, "the bottom-right corner aims BottomRight");

    // The band is inclusive of its far boundary: a corner wins over an edge, and
    // a position exactly `edge` inside aims at a tile while one past it does not.
    expect_tile(tile_target_for_cursor(screen, edge, 360, edge), WindowTileType::Left, "x == edge is still the left band");
    expect_tile(tile_target_for_cursor(screen, edge + 1, 360, edge), WindowTileType::None, "x just past the band is not");
    expect_tile(tile_target_for_cursor(screen, edge, edge, edge), WindowTileType::TopLeft, "the corner wins where two bands meet");
    expect_tile(tile_target_for_cursor(screen, 1280 - edge, 360, edge), WindowTileType::Right, "x == right - edge is the right band");
    expect_tile(tile_target_for_cursor(screen, 1280 - edge - 1, 360, edge), WindowTileType::None, "x just inside it is not");

    // A zero threshold is the degenerate case: only the outermost pixel counts.
    expect_tile(tile_target_for_cursor(screen, 0, 360, 0), WindowTileType::Left, "with no band, x == area.x() still aims Left");
    expect_tile(tile_target_for_cursor(screen, 1, 360, 0), WindowTileType::None, "with no band, one pixel in aims at nothing");

    // The area need not start at the origin (a panel reserves the top). The band
    // reaches *outward* from the usable area, so a drag above a panel is still a
    // drag to the top edge — the position is bounded by the band, not clipped to
    // the area.
    Gfx::IntRect const below_panel { 0, 32, 1280, 688 };
    expect_tile(tile_target_for_cursor(below_panel, 640, 30, edge), WindowTileType::Top, "above the usable area is still the top band");
    expect_tile(tile_target_for_cursor(below_panel, 640, 32 + edge, edge), WindowTileType::Top, "the band's inner boundary is inclusive");
    expect_tile(tile_target_for_cursor(below_panel, 640, 32 + edge + 1, edge), WindowTileType::None, "one pixel inside it is not");

    outln("=== window_visible_on_workspace ===");
    expect(window_visible_on_workspace(0, 0, 0, 0, true), "a mapped window on the current workspace is visible");
    expect(!window_visible_on_workspace(0, 1, 0, 0, true), "a window on another column is hidden");
    expect(!window_visible_on_workspace(1, 0, 0, 0, true), "a window on another row is hidden");
    expect(!window_visible_on_workspace(0, 0, 0, 0, false), "an unmapped window is hidden even on the current workspace");
    expect(window_visible_on_workspace(1, 1, 0, 0, true, true), "an override-redirect window is visible on any workspace");
    expect(!window_visible_on_workspace(1, 1, 0, 0, false, true), "an unmapped override-redirect window is still hidden");
    expect(window_visible_on_workspace(2, 3, 2, 3, true), "a window on the current 2,3 workspace is visible");

    outln("=== the window list (Alt-Tab switcher) ===");
    // An xdg window on the current stack is listed on either setting.
    SwitcherWindow const xdg_current { 0, 0, false, true, false, true };
    expect(switcher_lists_window(xdg_current, true, 0, 0), "an xdg window on the current stack is listed");
    expect(switcher_lists_window(xdg_current, false, 1, 1), "and with every stack listed");
    // An xdg window on another stack is filtered only when the list is filtered.
    SwitcherWindow const xdg_other { 1, 0, false, true, false, true };
    expect(!switcher_lists_window(xdg_other, true, 0, 0), "an xdg window on another stack is not listed when filtering");
    expect(switcher_lists_window(xdg_other, false, 0, 0), "but is listed with every stack");
    // An X11 window needs to be mapped and to have a Taskbar entry, and it is
    // filtered by stack like an xdg window.
    SwitcherWindow const x11_current { 0, 0, true, true, false, true };
    expect(switcher_lists_window(x11_current, true, 0, 0), "a mapped X11 window with a Taskbar entry is listed");
    expect(!switcher_lists_window({ 0, 0, true, false, false, true }, true, 0, 0), "an unmapped X11 window is not listed");
    expect(!switcher_lists_window({ 0, 0, true, true, false, false }, true, 0, 0), "an X11 window with no Taskbar entry is not listed");
    expect(!switcher_lists_window({ 1, 0, true, true, false, true }, true, 0, 0), "an X11 window on another stack is filtered like an xdg one");
    // An xdg window is listed even without an xdg-side "mapped" flag here: the
    // rule only consults mapping for X11 (the toplevel exists iff it is a window).
    expect(switcher_lists_window({ 0, 0, false, false, false, false }, true, 0, 0), "an xdg window is listed regardless of the X11-only fields");
    // Override-redirect windows are never listed, on any setting or stack.
    expect(!switcher_lists_window({ 0, 0, true, true, true, true }, true, 0, 0), "an override-redirect X11 window is never listed");
    expect(!switcher_lists_window({ 0, 0, true, true, true, true }, false, 0, 0), "not even with every stack listed");

    outln("=== the multi-stack indicator ===");
    expect(!switcher_spans_stacks({}), "an empty list does not span stacks");
    expect(!switcher_spans_stacks({ xdg_current }), "a single window does not span stacks");
    expect(!switcher_spans_stacks({ xdg_current, { 0, 0, true, true, false, true } }), "two windows on one stack do not span stacks");
    expect(switcher_spans_stacks({ xdg_current, xdg_other }), "windows on two stacks span stacks");
    expect(switcher_spans_stacks({ xdg_current, { 0, 1, false, true, false, true } }), "so do two columns of the same row");

    outln("=== the window-stack overlay ===");
    Gfx::IntRect const screen2 { 0, 0, 1280, 720 };
    // A 2x2 grid of 40x30 cells with 8px padding and a 16px margin, centred.
    auto const frame = stack_overlay_frame(screen2, 2, 2);
    int const content_w = 2 * (40 + 8) - 8; // 88
    int const content_h = 2 * (30 + 8) - 8; // 68
    expect_rect(frame, { (1280 - (content_w + 32)) / 2, (720 - (content_h + 32)) / 2, content_w + 32, content_h + 32 },
        "the overlay frame is the grid plus its padding and margin, centred");

    // Cell coordinates are in the overlay's own space (0,0 is its top-left), which
    // is what the painter draws in; the margin is the first cell's inset.
    auto const cell_00 = stack_overlay_cell(2, 2, 0, 0);
    auto const cell_01 = stack_overlay_cell(2, 2, 0, 1);
    auto const cell_10 = stack_overlay_cell(2, 2, 1, 0);
    expect_rect(cell_00, { 16, 16, 40, 30 }, "the first cell sits at the margin inside the overlay");
    expect(cell_01.x() == cell_00.x() + 40 + 8 && cell_01.y() == cell_00.y(), "the next column is one cell plus the padding to the right");
    expect(cell_10.y() == cell_00.y() + 30 + 8 && cell_10.x() == cell_00.x(), "the next row is one cell plus the padding down");
    // Every cell is inside the overlay for its grid size, and the padding is
    // between cells, not around the outside of them (it is already in the frame).
    for (int rows : { 1, 2, 4 }) {
        for (int columns : { 1, 2, 4 }) {
            auto const size = stack_overlay_frame({}, rows, columns).size();
            Gfx::IntRect const overlay { {}, size };
            bool inside = true;
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < columns; ++x)
                    inside = inside && overlay.contains(stack_overlay_cell(rows, columns, y, x));
            expect(inside, "every cell is inside the overlay for its grid size");
        }
    }
    // A usable area that is not at the origin (a panel) still centres inside it.
    auto const below = stack_overlay_frame({ 0, 32, 1280, 688 }, 2, 2);
    expect(below.y() >= 32, "the overlay centres inside the usable area, not the output");

    outln("=== the stack transition ===");
    Gfx::IntSize const area_size { 1280, 720 };
    expect(stack_transition_delta(0, 0, 0, 0, area_size) == Gfx::IntPoint { 0, 0 }, "switching to the same stack animates nothing");
    expect(stack_transition_delta(0, 1, 0, 0, area_size) == Gfx::IntPoint { -1280, 0 }, "moving right slides the old stack left");
    expect(stack_transition_delta(0, 0, 0, 1, area_size) == Gfx::IntPoint { 1280, 0 }, "moving left slides it right");
    expect(stack_transition_delta(1, 0, 0, 0, area_size) == Gfx::IntPoint { 0, -720 }, "moving down slides it up");
    expect(stack_transition_delta(0, 0, 1, 0, area_size) == Gfx::IntPoint { 0, 720 }, "moving up slides it down");
    // Diagonally across the grid, both axes move.
    expect(stack_transition_delta(1, 1, 0, 0, area_size) == Gfx::IntPoint { -1280, -720 }, "a diagonal switch moves both axes");
    expect(stack_transition_delta(0, 0, 1, 1, area_size) == Gfx::IntPoint { 1280, 720 }, "and back the other way");

    outln("=== the transition easing ===");
    expect(stack_transition_eased(0.0f) == 0.0f, "no time is no progress");
    expect(stack_transition_eased(1.0f) == 1.0f, "full time is full progress");
    expect(stack_transition_eased(0.5f) == 0.75f, "half the time is three quarters of the way (ease-out)");
    expect(stack_transition_eased(2.0f) == 1.0f, "past the end is clamped to the end");
    expect(stack_transition_eased(-1.0f) == 0.0f, "before the start is clamped to the start");
    // The easing is monotonic: it never moves backwards.
    bool monotonic = true;
    float previous = 0.0f;
    for (int i = 0; i <= 100; ++i) {
        float const eased = stack_transition_eased(static_cast<float>(i) / 100.0f);
        monotonic = monotonic && eased >= previous;
        previous = eased;
    }
    expect(monotonic, "the easing never goes backwards");
    // The lerp reaches its endpoints exactly and rounds to the nearest.
    expect(stack_transition_lerp(0, 100, 0.0f) == 0, "the lerp starts at the start");
    expect(stack_transition_lerp(0, 100, 1.0f) == 100, "the lerp ends at the end");
    expect(stack_transition_lerp(0, 100, 0.5f) == 50, "the lerp rounds to the nearest position");
    // The rounding is the compositor's: it adds a half and truncates, so the
    // negative direction lands one pixel past the midpoint (51, not 50). The step
    // is a pixel either way, but the test states the real behaviour rather than
    // the tidy one.
    expect(stack_transition_lerp(100, 0, 0.5f) == 51, "and works in the negative direction");
    expect(stack_transition_lerp(-10, 10, 0.5f) == 0, "and across zero");

    if (g_failures != 0) {
        outln("==> window rules test FAILED ({} check(s))", g_failures);
        return 1;
    }
    outln("==> window rules test PASSED");
    return 0;
}
