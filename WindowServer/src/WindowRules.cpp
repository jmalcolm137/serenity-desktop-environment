/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowRules.h"

namespace WindowServer {

Gfx::IntRect tile_rect(Gfx::IntRect const& area, WindowTileType type)
{
    Gfx::IntRect rect = area;
    int const half_w = area.width() / 2;
    int const half_h = area.height() / 2;
    switch (type) {
    case WindowTileType::Left:
        rect.set_width(half_w);
        break;
    case WindowTileType::Right:
        rect.set_x(area.x() + half_w);
        rect.set_width(area.width() - half_w);
        break;
    case WindowTileType::Top:
        rect.set_height(half_h);
        break;
    case WindowTileType::Bottom:
        rect.set_y(area.y() + half_h);
        rect.set_height(area.height() - half_h);
        break;
    case WindowTileType::TopLeft:
        rect.set_size({ half_w, half_h });
        break;
    case WindowTileType::TopRight:
        rect.set_x(area.x() + half_w);
        rect.set_size({ area.width() - half_w, half_h });
        break;
    case WindowTileType::BottomLeft:
        rect.set_y(area.y() + half_h);
        rect.set_size({ half_w, area.height() - half_h });
        break;
    case WindowTileType::BottomRight:
        rect.set_location({ area.x() + half_w, area.y() + half_h });
        rect.set_size({ area.width() - half_w, area.height() - half_h });
        break;
    default:
        break; // Maximized / horizontally / vertically maximized = the whole area
    }
    return rect;
}

WindowTileType tile_target_for_cursor(Gfx::IntRect const& area, double x, double y, int edge_threshold)
{
    bool const at_left = x <= area.x() + edge_threshold;
    bool const at_right = x >= area.right() - edge_threshold;
    bool const at_top = y <= area.y() + edge_threshold;
    bool const at_bottom = y >= area.bottom() - edge_threshold;

    if (at_left) {
        if (at_top)
            return WindowTileType::TopLeft;
        if (at_bottom)
            return WindowTileType::BottomLeft;
        return WindowTileType::Left;
    }
    if (at_right) {
        if (at_top)
            return WindowTileType::TopRight;
        if (at_bottom)
            return WindowTileType::BottomRight;
        return WindowTileType::Right;
    }
    if (at_top)
        return WindowTileType::Top;
    if (at_bottom)
        return WindowTileType::Bottom;
    return WindowTileType::None;
}

bool window_visible_on_workspace(u32 window_row, u32 window_column, u32 current_row, u32 current_column, bool mapped, bool override_redirect)
{
    if (!mapped)
        return false;
    if (override_redirect)
        return true;
    return window_row == current_row && window_column == current_column;
}

}
