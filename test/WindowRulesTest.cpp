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

    if (g_failures != 0) {
        outln("==> window rules test FAILED ({} check(s))", g_failures);
        return 1;
    }
    outln("==> window rules test PASSED");
    return 0;
}
