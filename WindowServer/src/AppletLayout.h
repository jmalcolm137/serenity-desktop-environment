/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>

// The Taskbar's applet area: the order its applets appear in and the rectangle
// each one gets. Pure functions of the applet sizes and the configured order, so
// the layout can be unit-tested (test/AppletLayoutTest.cpp) instead of only being
// visible by running a session and counting widgets.
namespace AppletLayout {

// The spacing between applets, in pixels (SerenityOS's Taskbar uses 4).
inline constexpr int default_spacing = 4;

// One applet as the layout sees it: its name (for the configured order) and the
// size its surface asked for.
struct Applet {
    ByteString name;
    Gfx::IntSize size;
};

// Where one applet goes: which input it is, and its rectangle relative to the
// applet area's origin (so the caller only adds the origin it has).
struct Placement {
    size_t applet_index { 0 };
    Gfx::IntRect rect;
};

struct Layout {
    Vector<Placement> placements;
    Gfx::IntSize area_size;
};

// SerenityOS's applet order: descending by the applet's index in the configured
// order, with applets that are not listed sorting last. Ties keep the order they
// were given in (the sort is stable), and the layout is right-to-left with
// `spacing` between neighbours and each applet centred vertically in the area.
Layout layout(Vector<Applet> const& applets, Vector<ByteString> const& configured_order, int area_height, int spacing = 4);

// Where the applet area sits when the Taskbar has not positioned it yet: the
// bottom-right corner of the usable area, which is what it falls back to before
// the first report.
Gfx::IntPoint fallback_origin(Gfx::IntRect const& usable_area, Gfx::IntSize area_size);

// Move an applet-area origin from one anchor geometry to another, keeping its
// distance from the anchor's bottom-right corner. The anchor is the Taskbar (or,
// before it reports, the usable area); when the resolution or output scale
// changes the anchor moves/resizes, and the applet area has to move with it
// instead of staying at the absolute position it was first given.
Gfx::IntPoint translate_with_anchor(Gfx::IntRect const& old_anchor, Gfx::IntRect const& new_anchor, Gfx::IntPoint origin);

}
