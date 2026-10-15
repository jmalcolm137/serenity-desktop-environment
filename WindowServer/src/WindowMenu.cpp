/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Focus.h"
#include "Frame.h"
#include "Resources.h"
#include "WindowMenu.h"
#include "XdgShell.h"

#include <AK/StdLibExtras.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <unistd.h>

#include "Wlroots.h"

extern "C" {
#include "serenity-window-server-protocol.h"
}

static int window_menu_item_height()
{
    auto& font = Gfx::FontDatabase::default_font();
    return max(static_cast<int>(ceilf(font.preferred_line_height())), 18) + 4;
}

static int window_menu_index_at(WindowMenu const& menu, double x, double y)
{
    if (!menu.rect.contains({ static_cast<int>(x), static_cast<int>(y) }))
        return -1;
    Gfx::IntPoint local { static_cast<int>(x) - menu.rect.x(), static_cast<int>(y) - menu.rect.y() };
    for (size_t i = 0; i < menu.entries.size(); ++i) {
        if (menu.entries[i].rect.contains(local))
            return static_cast<int>(i);
    }
    return -1;
}

static void paint_window_menu(Server& server, WindowMenu& menu)
{
    constexpr int frame = 2;
    constexpr int stripe_width = 24;
    auto& font = Gfx::FontDatabase::default_font();

    if (!menu.node)
        menu.node = make<ChromeNode>(*server.overlay_tree);
    menu.node->set_rect(menu.rect);
    menu.node->render([&](Gfx::Painter& painter) {
        Gfx::IntRect rect { { }, menu.rect.size() };
        painter.draw_rect(rect, Gfx::Color::Black);
        painter.fill_rect(rect.shrunken(2, 2), g_palette->menu_base());
        painter.fill_rect(Gfx::IntRect { frame, frame, stripe_width, menu.rect.height() - frame * 2 }, g_palette->menu_stripe());

        for (size_t i = 0; i < menu.entries.size(); ++i) {
            auto& entry = menu.entries[i];
            if (entry.is_separator) {
                int y = entry.rect.center().y() - 1;
                Gfx::IntPoint p1 { stripe_width + frame + 4, y };
                Gfx::IntPoint p2 { menu.rect.width() - 7, y };
                painter.draw_line(p1, p2, g_palette->threed_shadow1());
                painter.draw_line(p1.translated(0, 1), p2.translated(0, 1), g_palette->threed_highlight());
                continue;
            }

            bool hovered = static_cast<int>(i) == menu.hovered_index && entry.enabled;
            Gfx::Color text_color = g_palette->menu_base_text();
            if (hovered) {
                painter.fill_rect(entry.rect, g_palette->menu_selection());
                painter.draw_rect(entry.rect, g_palette->menu_selection().darkened());
                text_color = g_palette->menu_selection_text();
            } else if (!entry.enabled) {
                text_color = Gfx::Color::MidGray;
            }

            auto text_rect = entry.rect.translated(stripe_width + 6, 0);

            if (entry.checkable) {
                // Checkable items show a check mark in the stripe rather than an
                // icon, matching SerenityOS's window menus.
                Gfx::IntRect check_rect { entry.rect.x() + 6, 0, 12, 12 };
                check_rect.center_vertically_within(text_rect);
                if (entry.checked)
                    painter.draw_text(check_rect, "\xE2\x9C\x93"sv, Gfx::TextAlignment::Center, text_color);
            } else if (entry.icon) {
                Gfx::IntRect icon_rect { entry.rect.x() + 3, 0, entry.icon->width(), entry.icon->height() };
                icon_rect.center_vertically_within(text_rect);
                if (entry.enabled)
                    painter.blit(icon_rect.location(), *entry.icon, entry.icon->rect());
                else
                    painter.blit_disabled(icon_rect.location(), *entry.icon, entry.icon->rect(), *g_palette);
            }

            auto const& use_font = entry.is_default ? font.bold_variant() : font;
            painter.draw_ui_text(text_rect, entry.text, use_font, Gfx::TextAlignment::CenterLeft, text_color);
        }
    });
}

static void layout_window_menu(Server& server, WindowMenu& menu)
{
    auto& font = Gfx::FontDatabase::default_font();
    constexpr int frame = 2;
    constexpr int stripe_width = 24;
    constexpr int horizontal_padding = 28;
    int item_height = window_menu_item_height();

    int widest = 0;
    for (auto& entry : menu.entries) {
        if (entry.is_separator)
            continue;
        Optional<size_t> underline_offset;
        auto parsed = Gfx::parse_ampersand_string(entry.text, &underline_offset);
        widest = max(widest, font.width(parsed));
    }

    int width = widest + stripe_width + horizontal_padding + frame * 2;
    int next_y = frame;
    for (auto& entry : menu.entries) {
        int height = entry.is_separator ? 8 : item_height;
        entry.rect = { frame, next_y, width - frame * 2, height };
        next_y += height;
    }
    int height = next_y + frame;

    Gfx::IntRect output_box { 0, 0, width, height };
    if (server.primary_output) {
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
        output_box = { box.x, box.y, box.width, box.height };
    }
    int x = clamp(menu.rect.x(), output_box.x(), max(output_box.x(), output_box.right() - width));
    int y = clamp(menu.rect.y(), output_box.y(), max(output_box.y(), output_box.bottom() - height));
    menu.rect = { x, y, width, height };
}

void close_window_menu(Server& server)
{
    auto& menu = server.window_menu;
    if (!menu.open)
        return;
    menu.open = false;
    menu.ignore_release = false;
    menu.toplevel = nullptr;
    menu.hovered_index = -1;
    menu.pressed_index = -1;
    menu.entries.clear_with_capacity();
    if (menu.node)
        menu.node->set_visible(false);
}

void open_window_menu(Server& server, Toplevel& toplevel, Gfx::IntPoint position)
{
    close_window_menu(server);

    auto& menu = server.window_menu;
    menu.open = true;
    menu.ignore_release = true;
    // Opening the menu must not leave a stale frame grab behind: the press that
    // opened it is consumed here, not by an interactive frame drag.
    server.grab.from_frame = false;
    menu.toplevel = &toplevel;
    menu.hovered_index = -1;
    menu.pressed_index = -1;
    menu.entries.clear_with_capacity();

    auto add_item = [&](ByteString text, WindowMenuAction action, bool enabled, RefPtr<Gfx::Bitmap> icon = nullptr, bool is_default = false, bool checkable = false, bool checked = false) {
        WindowMenuEntry entry;
        entry.text = move(text);
        entry.action = action;
        entry.enabled = enabled;
        entry.icon = move(icon);
        entry.is_default = is_default;
        entry.checkable = checkable;
        entry.checked = checked;
        menu.entries.append(move(entry));
    };
    auto add_separator = [&] {
        WindowMenuEntry entry;
        entry.is_separator = true;
        menu.entries.append(move(entry));
    };

    add_item(toplevel.minimized ? ByteString { "&Unminimize" } : ByteString { "Mi&nimize" }, WindowMenuAction::MinimizeOrUnminimize, true);
    add_item(toplevel.maximized ? ByteString { "&Restore" } : ByteString { "Ma&ximize" }, WindowMenuAction::MaximizeOrRestore, true);
    add_item(ByteString { "&Move" }, WindowMenuAction::Move, !toplevel.minimized && !toplevel.maximized, g_frame_icons.move);
    add_separator();
    add_item(ByteString { "&Roll Up" }, WindowMenuAction::ToggleWindowRollUp, true, nullptr, false, true, toplevel.rolled_up);
    add_item(ByteString { "Menu &Bar" }, WindowMenuAction::ToggleMenubarVisibility, true, nullptr, false, true, toplevel.menubar_visible);
    add_separator();
    add_item(ByteString { "Always on &Top" }, WindowMenuAction::ToggleAlwaysOnTop, true, g_frame_icons.pin, false, true, toplevel.always_on_top);
    add_separator();
    add_item(ByteString { "&Close" }, WindowMenuAction::Close, true, g_frame_icons.close, true);

    menu.rect = { position.x(), position.y(), 0, 0 };
    layout_window_menu(server, menu);
    paint_window_menu(server, menu);

    menu.node->raise_to_top();
    menu.node->set_visible(true);
    dbgln("WindowServer: window menu opened for wm_id={}", toplevel.wm_id);
}

void update_window_menu_hover(Server& server, double x, double y)
{
    auto& menu = server.window_menu;
    int index = window_menu_index_at(menu, x, y);
    if (index == menu.hovered_index)
        return;
    menu.hovered_index = index;
    paint_window_menu(server, menu);
}

static void activate_window_menu_item(Server& server, int index)
{
    auto& menu = server.window_menu;
    if (index < 0 || index >= static_cast<int>(menu.entries.size()))
        return;
    auto entry = menu.entries[index];
    if (entry.is_separator || !entry.enabled)
        return;

    auto* toplevel = menu.toplevel;
    close_window_menu(server);
    if (!toplevel)
        return;

    switch (entry.action) {
    case WindowMenuAction::MinimizeOrUnminimize:
        toplevel_toggle_minimized(*toplevel);
        break;
    case WindowMenuAction::MaximizeOrRestore:
        focus_toplevel(server, toplevel);
        toplevel_toggle_maximized(*toplevel);
        break;
    case WindowMenuAction::Move:
        focus_toplevel(server, toplevel);
        toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
        break;
    case WindowMenuAction::ToggleWindowRollUp:
        toplevel->rolled_up = !toplevel->rolled_up;
        toplevel_update_frame(*toplevel);
        break;
    case WindowMenuAction::ToggleMenubarVisibility:
        toplevel->menubar_visible = !toplevel->menubar_visible;
        if (toplevel->chrome)
            serenity_toplevel_send_menubar_visibility(toplevel->chrome, toplevel->menubar_visible ? 1 : 0);
        break;
    case WindowMenuAction::ToggleAlwaysOnTop:
        toplevel->always_on_top = !toplevel->always_on_top;
        for (auto& candidate : server.toplevels) {
            if (candidate->always_on_top && !candidate->minimized)
                wlr_scene_node_raise_to_top(&candidate->scene_tree->node);
        }
        break;
    case WindowMenuAction::Close:
        toplevel_close(*toplevel);
        break;
    }
}

void window_menu_handle_press(Server& server, uint32_t button)
{
    auto& menu = server.window_menu;
    if (button == BTN_LEFT)
        menu.pressed_index = window_menu_index_at(menu, server.cursor->x, server.cursor->y);
    else
        close_window_menu(server);
}

void window_menu_handle_release(Server& server, uint32_t button)
{
    auto& menu = server.window_menu;
    if (!menu.open)
        return;

    // The release paired with the press that opened the menu must not dismiss
    // it, or it would vanish before an item could be chosen.
    if (menu.ignore_release) {
        menu.ignore_release = false;
        return;
    }

    if (button != BTN_LEFT) {
        close_window_menu(server);
        return;
    }
    int released = window_menu_index_at(menu, server.cursor->x, server.cursor->y);
    int pressed = menu.pressed_index;
    menu.pressed_index = -1;
    if (released >= 0 && released == pressed)
        activate_window_menu_item(server, released);
    else if (released < 0)
        close_window_menu(server);
}

bool window_menu_handle_key(Server& server, xkb_keysym_t sym)
{
    auto& menu = server.window_menu;
    if (!menu.open)
        return false;

    if (sym == XKB_KEY_Escape) {
        close_window_menu(server);
        return true;
    }
    if (sym == XKB_KEY_Up || sym == XKB_KEY_Down) {
        if (menu.entries.is_empty())
            return true;
        int count = static_cast<int>(menu.entries.size());
        int index = menu.hovered_index;
        for (int i = 0; i < count; ++i) {
            if (sym == XKB_KEY_Up)
                index = index <= 0 ? count - 1 : index - 1;
            else
                index = index >= count - 1 ? 0 : index + 1;
            auto const& entry = menu.entries[index];
            if (!entry.is_separator && entry.enabled)
                break;
        }
        menu.hovered_index = index;
        paint_window_menu(server, menu);
        return true;
    }
    if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter) {
        activate_window_menu_item(server, menu.hovered_index);
        return true;
    }
    return true;
}
