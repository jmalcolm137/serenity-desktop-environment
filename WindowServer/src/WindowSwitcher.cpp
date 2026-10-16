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
//
// xdg toplevels and X11 (Xwayland) windows share the list via WindowRef, so an
// X11 window is a first-class entry here (docs/20-xwayland-next-steps.md). X11
// windows are unmanaged by the workspace grid, so they list on the current stack.
namespace WindowSwitcher {

static HostHooks g_hooks;

void set_host_hooks(HostHooks hooks)
{
    g_hooks = hooks;
}

static bool s_visible { false };
static Mode s_mode { Mode::CurrentWorkspace };
static Vector<WindowRef> s_windows;
static Vector<RefPtr<Gfx::Bitmap>> s_thumbnails;
static Vector<ByteString> s_titles;
static int s_selected { 0 };
static int s_hovered { -1 };
static bool s_multi_stack { false };
static Gfx::IntRect s_rect;
static OwnPtr<ChromeNode> s_overlay;
static WindowRef s_highlight;

// SerenityOS WindowSwitcher geometry (padding()/item_padding()/item_height()).
static int const thumbnail_size = 64;
static int const padding = 30;
static int const item_padding = 10;

static int item_height() { return 14 + thumbnail_size; }
static Gfx::Font const& title_font() { return Gfx::FontDatabase::window_title_font(); }
static Gfx::Font const& details_font() { return Gfx::FontDatabase::default_font(); }

static WindowRef selected_window()
{
    if (s_selected < 0 || s_selected >= static_cast<int>(s_windows.size()))
        return {};
    return s_windows[s_selected];
}

// --- WindowRef accessors: an xdg toplevel or an X11 (Xwayland) window. --------

static ByteString window_title(WindowRef const& window)
{
    if (window.is_x11())
        return window.x11->title.is_empty() ? ByteString { "Untitled" } : window.x11->title;
    if (window.xdg->xdg_toplevel->title)
        return ByteString { window.xdg->xdg_toplevel->title, strlen(window.xdg->xdg_toplevel->title) };
    return ByteString { "Untitled" };
}

static Gfx::IntRect window_rect(WindowRef const& window)
{
    if (window.is_x11()) {
        auto& toplevel = *window.x11;
        auto* node = &toplevel.scene_tree->node;
        return { static_cast<int>(node->x) - toplevel.frame_left,
            static_cast<int>(node->y) - toplevel.frame_top,
            toplevel.frame_size.width(), toplevel.frame_size.height() };
    }
    auto& toplevel = *window.xdg;
    return { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y,
        toplevel.frame_size.width(), toplevel.frame_size.height() };
}

// The scene node whose region is snapshotted for the live thumbnail. For an xdg
// window that is the content (no compositor frame); for an X11 window the whole
// tree, so its server-side frame is included.
static wlr_scene_node* window_thumbnail_node(WindowRef const& window)
{
    return window.is_x11() ? &window.x11->scene_tree->node : &window.xdg->content_tree->node;
}

static Gfx::Bitmap const* window_icon(WindowRef const& window)
{
    return window.is_x11() ? window.x11->icon.ptr() : window.xdg->icon.ptr();
}

static u64 window_focus_order(WindowRef const& window)
{
    return window.is_x11() ? window.x11->focus_order : window.xdg->focus_order;
}

static u32 window_workspace_row(WindowRef const& window)
{
    return window.is_x11() ? window.x11->workspace_row : window.xdg->workspace_row;
}

static u32 window_workspace_column(WindowRef const& window)
{
    return window.is_x11() ? window.x11->workspace_column : window.xdg->workspace_column;
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
    s_titles.clear();

    u32 const row = WindowManager::current_workspace_row();
    u32 const column = WindowManager::current_workspace_column();
    bool const current_only = s_mode == Mode::CurrentWorkspace;

    // Which windows are listed, and why, is WindowRules.h's rule: an xdg toplevel
    // is listed whenever it exists as one, an X11 window only when it is mapped
    // and has a Taskbar entry, and an override-redirect window (a menu, a tooltip)
    // never.
    for (auto& toplevel : server.toplevels) {
        WindowServer::SwitcherWindow const window { toplevel->workspace_row, toplevel->workspace_column, false, true, false, true };
        if (WindowServer::switcher_lists_window(window, current_only, row, column))
            s_windows.append({ toplevel.ptr(), nullptr });
    }
    for (auto& toplevel : server.x11_toplevels) {
        WindowServer::SwitcherWindow const window { toplevel->workspace_row, toplevel->workspace_column, true, toplevel->mapped, toplevel->override_redirect, toplevel->wm_id != 0 };
        if (WindowServer::switcher_lists_window(window, current_only, row, column))
            s_windows.append({ nullptr, toplevel.ptr() });
    }

    // Most-recently-focused first, so the first Alt+Tab lands on the
    // previously-focused window (Serenity's window stack order).
    insertion_sort(s_windows, [](WindowRef const& a, WindowRef const& b) { return window_focus_order(a) > window_focus_order(b); });

    // Multi-stack indicator: do the listed windows span more than one stack?
    Vector<WindowServer::SwitcherWindow> listed;
    listed.ensure_capacity(s_windows.size());
    for (auto const& window : s_windows)
        listed.append({ window_workspace_row(window), window_workspace_column(window) });
    s_multi_stack = WindowServer::switcher_spans_stacks(listed);

    for (auto const& window : s_windows) {
        s_titles.append(window_title(window));
        s_thumbnails.append(snapshot_toplevel(*window_thumbnail_node(window)));
    }
}

// Draw the selected window's frame in the theme's "Highlighted" state, like
// SerenityOS WindowManager::set_highlight_window.
static void update_highlight(WindowRef next)
{
    if (next.xdg == s_highlight.xdg && next.x11 == s_highlight.x11)
        return;
    auto const previous = s_highlight;
    s_highlight = next;
    if (previous.is_x11()) {
        if (g_hooks.update_x11_frame)
            g_hooks.update_x11_frame(*previous.x11);
    } else if (previous.xdg && g_hooks.update_frame) {
        g_hooks.update_frame(*previous.xdg);
    }
    if (next.is_x11()) {
        if (g_hooks.update_x11_frame)
            g_hooks.update_x11_frame(*next.x11);
    } else if (next.xdg && g_hooks.update_frame) {
        g_hooks.update_frame(*next.xdg);
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
    auto const window = selected_window();
    if (s_mode == Mode::AllWorkspaces && g_hooks.switch_workspace && window.is_valid())
        g_hooks.switch_workspace(server, window_workspace_row(window), window_workspace_column(window), false);
    update_highlight(window);
}

static void render(Server& server)
{
    if (s_windows.is_empty() || !server.overlay_tree)
        return;

    int longest_title = 0;
    for (auto const& title : s_titles)
        longest_title = max(longest_title, title_font().width(title));

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
    auto const titles = s_titles;
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
            auto const& window = windows[index];
            if (index < thumbnails.size() && thumbnails[index])
                painter.draw_scaled_bitmap(thumb, *thumbnails[index], thumbnails[index]->rect(), 1.0f, Gfx::ScalingMode::BilinearBlend);
            else
                painter.fill_rect(thumb, palette.base());

            // The window icon, bottom-right of the thumbnail (when the client supplies one).
            if (auto const* icon = window_icon(window)) {
                Gfx::IntRect icon_rect {
                    thumb.bottom_right().translated(-icon->width() - 1, -icon->height() - 1),
                    { icon->width(), icon->height() },
                };
                painter.blit(icon_rect.location(), *icon, icon->rect());
            }

            auto const& title = index < titles.size() ? titles[index] : window_title(window);
            auto const title_rect = item.translated(thumbnail_size + 12, 0);
            painter.draw_text(title_rect.translated(1, 0), title, title_font(), Gfx::TextAlignment::CenterLeft, text_color.inverted());
            painter.draw_text(title_rect, title, title_font(), Gfx::TextAlignment::CenterLeft, text_color);

            auto details = window_rect(window).to_byte_string();
            if (multi)
                details = ByteString::formatted("{} on {}:{}", details, window_workspace_row(window) + 1, window_workspace_column(window) + 1);
            painter.draw_text(item, details, details_font(), Gfx::TextAlignment::CenterRight, rect_text_color);
        }
    });
    s_overlay->set_visible(true);
    s_overlay->raise_to_top();
}

static void activate(Server& server)
{
    auto const window = selected_window();
    if (window.is_x11()) {
        dbgln("WindowSwitcher: activating X11 window wm_id={}", window.x11->wm_id);
        if (s_mode == Mode::AllWorkspaces && g_hooks.switch_workspace)
            g_hooks.switch_workspace(server, window.x11->workspace_row, window.x11->workspace_column, false);
        if (g_hooks.focus_x11)
            g_hooks.focus_x11(server, window.x11);
    } else if (window.xdg) {
        auto* toplevel = window.xdg;
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
        if (s_windows.is_empty()) {
            // Say so. Returning silently here made a dead client look like a broken
            // switcher: the "showing N windows" line the tests look for is emitted
            // below, so an empty list reads as "the switcher never ran".
            dbgln("WindowSwitcher: nothing to show (mode={})", to_underlying(mode));
            return;
        }
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
    return s_highlight.xdg;
}

X11Toplevel* highlighted_x11()
{
    return s_highlight.x11;
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
    auto const previous = selected_window();
    build_list(server);
    if (s_windows.is_empty()) {
        hide();
        return;
    }
    if (previous.is_valid()) {
        for (size_t i = 0; i < s_windows.size(); ++i) {
            if (s_windows[i].xdg == previous.xdg && s_windows[i].x11 == previous.x11) {
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
    update_highlight({});
    if (s_overlay)
        s_overlay->set_visible(false);
    s_windows.clear();
    s_thumbnails.clear();
    s_titles.clear();
    s_hovered = -1;
}

}
