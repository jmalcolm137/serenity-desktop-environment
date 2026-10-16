/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Unit tests for the Taskbar's applet area layout (AppletLayout.h): the order
// applets appear in and the rectangle each one gets. Pure functions of the applet
// sizes and the configured order, so the layout is checked here rather than by
// starting a session and counting widgets.

#include <AK/Format.h>
#include <LibMain/Main.h>

#include "AppletLayout.h"

using namespace AppletLayout;

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

Applet applet(char const* name, int width, int height = 20)
{
    return { ByteString { name }, { width, height } };
}

// The names in the order the layout placed them, right-to-left.
Vector<ByteString> placed_names(Vector<Applet> const& applets, Layout const& layout)
{
    Vector<ByteString> names;
    for (auto const& placement : layout.placements)
        names.append(applets[placement.applet_index].name);
    return names;
}

}

ErrorOr<int> serenity_main(Main::Arguments)
{
    outln("=== the configured order ===");
    // Ascending Order means right-to-left placement, so the highest index ends up
    // leftmost (SerenityOS's Taskbar).
    Vector<Applet> const four { applet("Audio", 24), applet("Network", 20), applet("Clock", 60), applet("Battery", 18) };
    Vector<ByteString> const order { "Clock", "Network", "Audio", "Battery" };
    auto const laid_out = layout(four, order, 24);
    auto const names = placed_names(four, laid_out);
    expect(names.size() == 4, "all four applets are placed");
    expect(names[0] == "Battery", "the first configured applet is placed rightmost");
    expect(names[1] == "Audio", "then the next");
    expect(names[2] == "Network", "then the next");
    expect(names[3] == "Clock", "and the last configured applet is leftmost");

    // The order is by index in the configured list, so a name that is not in it
    // shares priority with the name at index 0: "unlisted" and "first in the
    // order" are the same thing to the layout. Among equals the sort is stable, so
    // the given order is kept. Both of those are the existing behaviour and are
    // pinned here so a change to either is deliberate.
    Vector<Applet> const unlisted { applet("A", 10), applet("Listed", 10), applet("B", 10) };
    // "B" is at index 1, so it outranks everything at index 0.
    auto const unlisted_layout = layout(unlisted, { "Unused", "B" }, 20);
    auto const unlisted_names = placed_names(unlisted, unlisted_layout);
    expect(unlisted_names.size() == 3, "all applets are placed");
    expect(unlisted_names[0] == "B", "a later-listed applet is placed first (rightmost)");
    expect(unlisted_names[1] == "A" && unlisted_names[2] == "Listed", "the rest keep the order they were given in (the sort is stable)");
    // An applet named at index 0 is indistinguishable from an unlisted one.
    auto const first_equals_unlisted = placed_names(unlisted, layout(unlisted, { "Listed" }, 20));
    expect(first_equals_unlisted[1] == "Listed", "an applet listed first keeps its given position (same priority as unlisted)");

    // An entirely empty configured order is still deterministic: the given order,
    // laid out right to left.
    auto const no_order = layout(four, {}, 24);
    auto const no_order_names = placed_names(four, no_order);
    expect(no_order_names[0] == "Audio" && no_order_names[3] == "Battery", "with no configuration the given order is kept (reversed by placement)");

    outln("=== the rectangles ===");
    // Three 20-wide applets, 4px apart: 20+4+20+4+20 = 68 wide.
    Vector<Applet> const three { applet("First", 20), applet("Second", 20), applet("Third", 20) };
    auto const three_layout = layout(three, { "First", "Second", "Third" }, 24);
    expect(three_layout.area_size.width() == 20 + 4 + 20 + 4 + 20, "the area is the applets plus the spacing between them");
    expect(three_layout.area_size.height() == 24, "the area is the configured height");
    // Placed right to left: First at the right edge, Third at the left.
    expect(three_layout.placements[0].rect.right() == three_layout.area_size.width(), "the first applet ends at the area's right edge");
    expect(three_layout.placements[0].rect.x() == 68 - 20, "and starts one applet-width to the left of it");
    expect(three_layout.placements[1].rect.x() == 68 - 20 - 4 - 20, "the next is one spacing and one width further left");
    expect(three_layout.placements[2].rect.x() == 0, "the last applet starts at the area's left edge");
    // No spacing is added after the last applet, and neighbours are exactly
    // `spacing` apart.
    expect(three_layout.placements[0].rect.x() - three_layout.placements[1].rect.right() == 4, "neighbours are one spacing apart");
    expect(three_layout.placements[2].rect.right() + 4 == three_layout.placements[1].rect.x(), "including the last pair");

    // Each applet is centred vertically in the area.
    Vector<Applet> const mixed { applet("Tall", 20, 24), applet("Short", 20, 10) };
    auto const mixed_layout = layout(mixed, { "Tall", "Short" }, 24);
    for (auto const& placement : mixed_layout.placements) {
        int const expected_y = (24 - placement.rect.height()) / 2;
        expect(placement.rect.y() == expected_y, "an applet is centred vertically in the area");
    }

    outln("=== degenerate sizes ===");
    // A zero (or negative) width still reserves the spacing, so the layout does
    // not collapse two applets onto each other.
    Vector<Applet> const zero_width { applet("Zero", 0), applet("Real", 20) };
    auto const zero_layout = layout(zero_width, { "Zero", "Real" }, 20);
    expect(zero_layout.area_size.width() == 0 + 4 + 20, "a zero-width applet reserves only the spacing");
    // "Real" is later in the order, so it is placed first (rightmost) and the
    // zero-width applet is leftmost with only its spacing reserved.
    expect(zero_layout.placements[0].rect.width() == 20 && zero_layout.placements[0].rect.x() == 4, "the real applet keeps its width, one spacing in from the left");
    expect(zero_layout.placements[1].rect.width() == 0 && zero_layout.placements[1].rect.x() == 0, "the zero-width applet is placed at the left edge");
    // One applet has no spacing at all.
    auto const single = layout({ applet("Only", 30) }, { "Only" }, 20);
    expect(single.area_size.width() == 30, "a single applet adds no spacing");
    expect(single.placements[0].rect.x() == 0, "and sits at the left edge of its own area");
    // No applets: an empty area, not a negative one.
    auto const none = layout({}, {}, 20);
    expect(none.placements.is_empty(), "no applets means no placements");
    expect(none.area_size.width() == 0, "and a zero-width area (never negative)");

    outln("=== the fallback origin ===");
    // Before the Taskbar reports a position, the area hangs off the usable area's
    // bottom-right corner.
    Gfx::IntRect const usable { 0, 32, 1280, 688 };
    auto const origin = fallback_origin(usable, { 68, 24 });
    expect(origin == Gfx::IntPoint { 1280 - 68, 720 - 24 }, "the fallback origin is the usable area's bottom-right corner");
    expect(origin.y() + 24 == 720, "the area's bottom edge is the usable area's bottom edge");

    if (g_failures != 0) {
        outln("==> applet layout test FAILED ({} check(s))", g_failures);
        return 1;
    }
    outln("==> applet layout test PASSED");
    return 0;
}
