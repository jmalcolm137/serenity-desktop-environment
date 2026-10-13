/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowSwitcher.h"

#include <AK/ByteString.h>
#include <AK/InsertionSort.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/StylePainter.h>
#include <xkbcommon/xkbcommon.h>

#include "Overlays.h"
#include "WindowManager.h"

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
}

// The Alt+Tab switcher, drawn to match SerenityOS's
// Userland/Services/WindowServer/WindowSwitcher.cpp: a floating, shadowed panel
// (the theme's overlay rect shadow) listing one window per row with a 64x64 live
// thumbnail, the window icon (when the client supplies one), the computed title,
// and the frame rect (+ "on row:col" across workspaces) right-aligned.
namespace WindowSwitcher {

static HostHooks g_hooks;

void set_host_hooks(HostHooks hooks)
{
    g_hooks = hooks;
}

static bool s_visible { false };
static Mode s_mode { Mode::CurrentWorkspace };
static Vector<Toplevel*> s_windows;
static Vector<RefPtr<Gfx::Bitmap>> s_thumbnails;
static int s_selected { 0 };
static int s_hovered { -1 };
static bool s_multi_stack { false };
static Gfx::IntRect s_rect;
static OwnPtr<ChromeNode> s_overlay;
static Toplevel* s_highlight { nullptr };

// SerenityOS WindowSwitcher geometry (padding()/item_padding()/item_height()).
static int const thumbnail_size = 64;
static int const padding = 30;
static int const item_padding = 10;

static int item_height() { return 14 + thumbnail_size; }
static Gfx::Font const& title_font() { return Gfx::FontDatabase::window_title_font(); }
static Gfx::Font const& details_font() { return Gfx::FontDatabase::default_font(); }

static Toplevel* selected_toplevel()
{
    if (s_selected < 0 || s_selected >= static_cast<int>(s_windows.size()))
        return nullptr;
    return s_windows[s_selected];
}

static StringView title_for(Toplevel const& toplevel)
{
    if (toplevel.xdg_toplevel->title)
        return StringView { toplevel.xdg_toplevel->title, strlen(toplevel.xdg_toplevel->title) };
    return "Untitled"sv;
}

static Gfx::IntRect toplevel_rect(Toplevel const& toplevel)
{
    return { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y,
        toplevel.frame_size.width(), toplevel.frame_size.height() };
}

static Gfx::IntRect output_rect(Server& server)
{
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
    return { box.x, box.y, box.width, box.height };
}

static Gfx::IntRect item_rect(int index)
{
    return { padding, padding + index * item_height(), s_rect.width() - padding * 2, item_height() };
}

static int index_at(double global_x, double global_y)
{
    Gfx::IntPoint local { static_cast<int>(global_x) - s_rect.x(), static_cast<int>(global_y) - s_rect.y() };
    for (size_t i = 0; i < s_windows.size(); ++i) {
        if (item_rect(static_cast<int>(i)).contains(local))
            return static_cast<int>(i);
    }
    return -1;
}

static void build_list(Server& server)
{
    s_windows.clear();
    s_thumbnails.clear();
    for (auto& toplevel : server.toplevels) {
        if (s_mode == Mode::CurrentWorkspace
            && (toplevel->workspace_row != WindowManager::current_workspace_row()
                || toplevel->workspace_column != WindowManager::current_workspace_column()))
            continue;
        s_windows.append(toplevel.ptr());
    }
    // Most-recently-focused first, so the first Alt+Tab lands on the
    // previously-focused window (Serenity's window stack order).
    insertion_sort(s_windows, [](Toplevel* a, Toplevel* b) { return a->focus_order > b->focus_order; });

    s_multi_stack = false;
    for (size_t i = 1; i < s_windows.size(); ++i) {
        if (s_windows[i]->workspace_row != s_windows[0]->workspace_row
            || s_windows[i]->workspace_column != s_windows[0]->workspace_column) {
            s_multi_stack = true;
            break;
        }
    }

    for (auto* toplevel : s_windows)
        s_thumbnails.append(snapshot_toplevel(toplevel->content_tree->node));
}

// Draw the selected window's frame in the theme's "Highlighted" state, like
// SerenityOS WindowManager::set_highlight_window.
static void update_highlight(Toplevel* next)
{
    if (next == s_highlight)
        return;
    auto* previous = s_highlight;
    s_highlight = next;
    if (g_hooks.update_frame) {
        if (previous)
            g_hooks.update_frame(*previous);
        if (s_highlight)
            g_hooks.update_frame(*s_highlight);
    }
}

static void step(int delta)
{
    int const count = static_cast<int>(s_windows.size());
    if (count == 0)
        return;
    s_selected = ((s_selected + delta) % count + count) % count;
}

// Select an index: in all-workspaces mode switch to the window's workspace live
// (SerenityOS switches the window stack as you cycle), then update the highlight.
static void select_index(Server& server, int index)
{
    s_selected = index;
    if (s_mode == Mode::AllWorkspaces && g_hooks.switch_workspace) {
        if (auto* toplevel = selected_toplevel())
            g_hooks.switch_workspace(server, toplevel->workspace_row, toplevel->workspace_column, false);
    }
    update_highlight(selected_toplevel());
}

static void render(Server& server)
{
    if (s_windows.is_empty() || !server.overlay_tree)
        return;

    int longest_title = 0;
    for (auto* toplevel : s_windows)
        longest_title = max(longest_title, title_font().width(title_for(*toplevel)));

    int const space_for_window_details = 200;
    int const width = thumbnail_size + longest_title + space_for_window_details + padding * 2 + item_padding * 2;
    int const height = static_cast<int>(s_windows.size()) * item_height() + padding * 2;

    auto const screen = output_rect(server);
    s_rect = Gfx::IntRect {
        screen.x() + (screen.width() - width) / 2,
        screen.y() + (screen.height() - height) / 2,
        width,
        height,
    };

    if (!s_overlay)
        s_overlay = make<ChromeNode>(*server.overlay_tree);
    s_overlay->set_rect(s_rect);

    if (!g_hooks.palette)
        return;
    auto const& palette = g_hooks.palette(server);
    Gfx::Bitmap const* shadow = g_hooks.overlay_shadow ? g_hooks.overlay_shadow() : nullptr;
    auto const windows = s_windows;
    auto const thumbnails = s_thumbnails;
    int const selected = s_selected;
    int const hovered = s_hovered;
    bool const multi = s_multi_stack;
    s_overlay->render([&](Gfx::Painter& painter) {
        Gfx::IntRect local { { }, s_rect.size() };
        painter.clear_rect(local, Gfx::Color::Transparent);
        // The theme's 9-slice overlay shadow (which also fills the panel body).
        if (shadow)
            Gfx::StylePainter::paint_simple_rect_shadow(painter, local, *shadow, true, true);
        else {
            painter.fill_rect(local, palette.window());
            painter.draw_rect(local, palette.threed_shadow1());
        }

        for (size_t index = 0; index < windows.size(); ++index) {
            auto item = item_rect(static_cast<int>(index));
            Gfx::Color text_color;
            Gfx::Color rect_text_color;
            if (static_cast<int>(index) == selected) {
                painter.fill_rect(item, palette.selection());
                text_color = palette.selection_text();
                rect_text_color = palette.selection_text().with_alpha(0xcc);
            } else {
                if (static_cast<int>(index) == hovered)
                    Gfx::StylePainter::paint_frame(painter, item, palette, Gfx::FrameStyle::RaisedPanel);
                text_color = Gfx::Color(Gfx::Color::White);
                rect_text_color = Gfx::Color(Gfx::Color::White).with_alpha(0xcc);
            }
            item.shrink(item_padding, 0);

            Gfx::IntRect thumb { item.location().translated(0, 5), { thumbnail_size, thumbnail_size } };
            auto const& toplevel = *windows[index];
            if (index < thumbnails.size() && thumbnails[index])
                painter.draw_scaled_bitmap(thumb, *thumbnails[index], thumbnails[index]->rect(), 1.0f, Gfx::ScalingMode::BilinearBlend);
            else
                painter.fill_rect(thumb, palette.base());

            // The window icon, bottom-right of the thumbnail (when the client supplies one).
            if (toplevel.icon) {
                Gfx::IntRect icon_rect {
                    thumb.bottom_right().translated(-toplevel.icon->width() - 1, -toplevel.icon->height() - 1),
                    { toplevel.icon->width(), toplevel.icon->height() },
                };
                painter.blit(icon_rect.location(), *toplevel.icon, toplevel.icon->rect());
            }

            auto const title_rect = item.translated(thumbnail_size + 12, 0);
            painter.draw_text(title_rect.translated(1, 0), title_for(toplevel), title_font(), Gfx::TextAlignment::CenterLeft, text_color.inverted());
            painter.draw_text(title_rect, title_for(toplevel), title_font(), Gfx::TextAlignment::CenterLeft, text_color);

            auto details = toplevel_rect(toplevel).to_byte_string();
            if (multi)
                details = ByteString::formatted("{} on {}:{}", details, toplevel.workspace_row + 1, toplevel.workspace_column + 1);
            painter.draw_text(item, details, details_font(), Gfx::TextAlignment::CenterRight, rect_text_color);
        }
    });
    s_overlay->set_visible(true);
    s_overlay->raise_to_top();
}

static void activate(Server& server)
{
    if (s_selected >= 0 && s_selected < static_cast<int>(s_windows.size())) {
        auto* toplevel = s_windows[s_selected];
        dbgln("WindowSwitcher: activating wm_id={}", toplevel->wm_id);
        if (s_mode == Mode::AllWorkspaces && g_hooks.switch_workspace)
            g_hooks.switch_workspace(server, toplevel->workspace_row, toplevel->workspace_column, false);
        if (toplevel->minimized)
            WindowManager::set_toplevel_minimized(server, *toplevel, false);
        if (g_hooks.focus_toplevel)
            g_hooks.focus_toplevel(server, toplevel);
    }
    hide();
}

static void show(Server& server, Mode mode, bool reverse)
{
    if (!s_visible) {
        s_mode = mode;
        build_list(server);
        if (s_windows.is_empty())
            return;
        s_visible = true;
        s_hovered = -1;
        s_selected = 0;
        // Advance once so the first Alt+Tab goes to the previous window.
        step(reverse ? -1 : 1);
        dbgln("WindowSwitcher: showing {} windows (mode={})", s_windows.size(), to_underlying(mode));
    } else {
        step(reverse ? -1 : 1);
    }
    select_index(server, s_selected);
    render(server);
}

bool is_visible()
{
    return s_visible;
}

Toplevel* highlighted()
{
    return s_highlight;
}

bool handle_pointer_motion(Server& server, double x, double y)
{
    if (!s_visible)
        return false;
    int const index = index_at(x, y);
    if (index != s_hovered) {
        s_hovered = index;
        render(server);
    }
    return true;
}

bool handle_pointer_button(Server& server, u32 button, u32 state)
{
    (void)button;
    if (!s_visible)
        return false;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
        int const index = index_at(server.cursor->x, server.cursor->y);
        if (index >= 0) {
            select_index(server, index);
            render(server);
            activate(server);
        }
    }
    return true;
}

bool handle_key(Server& server, u32 keysym, u32 modifiers, bool pressed)
{
    if (pressed) {
        if (keysym == XKB_KEY_Tab && (modifiers & (WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO))) {
            auto const mode = (modifiers & WLR_MODIFIER_LOGO) ? Mode::AllWorkspaces : Mode::CurrentWorkspace;
            show(server, mode, (modifiers & WLR_MODIFIER_SHIFT) != 0);
            return true;
        }
        if (s_visible) {
            // The switcher grabs the keyboard while visible. Shift is ignored;
            // any other key (Escape included) cancels, like SerenityOS.
            if (keysym != XKB_KEY_Shift_L && keysym != XKB_KEY_Shift_R)
                hide();
            return true;
        }
        return false;
    }

    // Releasing the modifier that opened it activates the selection.
    if (s_visible && (keysym == XKB_KEY_Alt_L || keysym == XKB_KEY_Alt_R)) {
        activate(server);
        // The client received the Alt *press* (it may have activated its menu
        // bar), so the release must reach it too or the client's keyboard state is
        // left unbalanced and its menu bar keeps keyboard focus.
        return false;
    }
    if (s_visible && (keysym == XKB_KEY_Super_L || keysym == XKB_KEY_Super_R)) {
        activate(server);
        // The Super press was consumed (WindowManager::handle_super_key), so the
        // release is consumed too, keeping the client balanced.
        return true;
    }
    return false;
}

void refresh(Server& server)
{
    if (!s_visible)
        return;
    // Keep the selection on the same window across the rebuild.
    auto* previous = selected_toplevel();
    build_list(server);
    if (s_windows.is_empty()) {
        hide();
        return;
    }
    if (previous) {
        for (size_t i = 0; i < s_windows.size(); ++i) {
            if (s_windows[i] == previous) {
                s_selected = static_cast<int>(i);
                break;
            }
        }
    }
    s_selected = min(s_selected, static_cast<int>(s_windows.size()) - 1);
    select_index(server, s_selected);
    render(server);
}

void hide()
{
    s_visible = false;
    update_highlight(nullptr);
    if (s_overlay)
        s_overlay->set_visible(false);
    s_windows.clear();
    s_thumbnails.clear();
    s_hovered = -1;
}

}
