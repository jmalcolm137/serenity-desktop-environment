/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AppletLayout.h"

#include <AK/InsertionSort.h>
#include <AK/StdLibExtras.h>

namespace AppletLayout {

Layout layout(Vector<Applet> const& applets, Vector<ByteString> const& configured_order, int area_height, int spacing)
{
    // Indices, so the caller keeps its own order and the sort is over a copy.
    Vector<size_t> order;
    order.ensure_capacity(applets.size());
    for (size_t i = 0; i < applets.size(); ++i)
        order.append(i);

    insertion_sort(order, [&](size_t a, size_t b) {
        auto index_a = configured_order.find_first_index(applets[a].name).value_or(0);
        auto index_b = configured_order.find_first_index(applets[b].name).value_or(0);
        return index_a > index_b;
    });

    int total_width = 0;
    for (size_t index : order)
        total_width += max(0, applets[index].size.width()) + spacing;
    if (!order.is_empty())
        total_width -= spacing; // No spacing after the last one.

    Layout result;
    result.area_size = { total_width, area_height };

    int right_edge_x = total_width;
    for (size_t index : order) {
        int const width = applets[index].size.width();
        int const height = applets[index].size.height();
        Gfx::IntRect rect { right_edge_x - width, 0, width, height };
        rect.center_vertically_within({ 0, 0, 0, area_height });
        result.placements.append({ index, rect });
        right_edge_x = rect.x() - spacing;
    }
    return result;
}

Gfx::IntPoint fallback_origin(Gfx::IntRect const& usable_area, Gfx::IntSize area_size)
{
    return { usable_area.right() - area_size.width(), usable_area.bottom() - area_size.height() };
}

}
