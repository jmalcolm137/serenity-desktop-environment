/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
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
#include <xkbcommon/xkbcommon.h>

#include "Overlays.h"
#include "WindowManager.h"

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
}

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
static OwnPtr<ChromeNode> s_overlay;

static int const thumbnail_size = 64;
static int const item_padding = 6;
static int const list_padding = 16;

static int item_height() { return thumbnail_size + item_padding * 2; }
static Gfx::Font const& font() { return Gfx::FontDatabase::default_font(); }

static StringView title_for(Toplevel const& toplevel)
{
    if (toplevel.xdg_toplevel->title)
        return StringView { toplevel.xdg_toplevel->title, strlen(toplevel.xdg_toplevel->title) };
    return "Untitled"sv;
}

static Gfx::IntRect output_rect(Server& server)
{
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
    return { box.x, box.y, box.width, box.height };
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
    // Most-recently-focused first, mirroring Serenity's window stack, so the
    // first Alt+Tab lands on the previously-focused window.
    insertion_sort(s_windows, [](Toplevel* a, Toplevel* b) { return a->focus_order > b->focus_order; });
    for (auto* toplevel : s_windows) {
        // Live thumbnail from the client's current buffer (falls back to nothing
        // for non-mappable buffers; the item then shows a plain background).
        s_thumbnails.append(snapshot_toplevel(toplevel->content_tree->node));
    }
}

static void step(int delta)
{
    int const count = static_cast<int>(s_windows.size());
    if (count == 0)
        return;
    s_selected = ((s_selected + delta) % count + count) % count;
}

static void render(Server& server)
{
    if (s_windows.is_empty() || !server.overlay_tree)
        return;

    int longest_title = 0;
    for (auto* toplevel : s_windows)
        longest_title = max(longest_title, font().width(title_for(*toplevel)));
    int const width = list_padding * 2 + thumbnail_size + item_padding * 2 + 16 + longest_title + 16 + 120;
    int const height = list_padding * 2 + static_cast<int>(s_windows.size()) * item_height();

    auto const screen = output_rect(server);
    Gfx::IntRect rect {
        screen.x() + (screen.width() - width) / 2,
        screen.y() + (screen.height() - height) / 2,
        width,
        height,
    };

    if (!s_overlay)
        s_overlay = make<ChromeNode>(*server.overlay_tree);
    s_overlay->set_rect(rect);

    if (!g_hooks.palette)
        return;
    auto const& palette = g_hooks.palette(server);
    auto const windows = s_windows;
    auto const thumbnails = s_thumbnails;
    int const selected = s_selected;
    s_overlay->render([&](Gfx::Painter& painter) {
        Gfx::IntRect local { { }, rect.size() };
        painter.fill_rect(local, palette.window());
        painter.draw_rect(local, palette.threed_shadow1());

        int y = list_padding;
        for (size_t i = 0; i < windows.size(); ++i) {
            Gfx::IntRect item { list_padding, y, rect.width() - list_padding * 2, item_height() };
            bool const is_selected = static_cast<int>(i) == selected;
            if (is_selected)
                painter.fill_rect(item, palette.selection());

            Gfx::IntRect thumb { item.x() + item_padding, item.y() + item_padding, thumbnail_size, thumbnail_size };
            if (i < thumbnails.size() && thumbnails[i])
                painter.draw_scaled_bitmap(thumb, *thumbnails[i], thumbnails[i]->rect());
            else
                painter.fill_rect(thumb, palette.base());

            auto const text_color = is_selected ? palette.selection_text() : palette.window_text();
            auto const title_rect = item.translated(thumbnail_size + item_padding * 3, 0);
            painter.draw_ui_text(title_rect, title_for(*windows[i]), font(), Gfx::TextAlignment::CenterLeft, text_color);
            auto const details = ByteString::formatted("{}x{}", windows[i]->frame_size.width(), windows[i]->frame_size.height());
            painter.draw_ui_text(item.translated(0, 0), details, font(), Gfx::TextAlignment::CenterRight, text_color);
            y += item_height();
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
        if (toplevel->minimized) {
            toplevel->minimized = false;
            WindowManager::update_toplevel_visibility(*toplevel);
        }
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
        s_selected = 0;
        // Advance once so the first Alt+Tab goes to the previous window.
        step(reverse ? -1 : 1);
        dbgln("WindowSwitcher: showing {} windows (mode={})", s_windows.size(), to_underlying(mode));
    } else {
        step(reverse ? -1 : 1);
    }
    render(server);
}

bool is_visible()
{
    return s_visible;
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
            // The switcher grabs the keyboard while visible.
            if (keysym == XKB_KEY_Escape)
                hide();
            return true;
        }
        return false;
    }

    // Releasing the modifier that opened it activates the selection.
    if (s_visible && (keysym == XKB_KEY_Alt_L || keysym == XKB_KEY_Alt_R || keysym == XKB_KEY_Super_L || keysym == XKB_KEY_Super_R)) {
        activate(server);
        return true;
    }
    return false;
}

void refresh(Server& server)
{
    if (!s_visible)
        return;
    // Keep the selection on the same window across the rebuild.
    auto* previous = (s_selected >= 0 && s_selected < static_cast<int>(s_windows.size())) ? s_windows[s_selected] : nullptr;
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
    render(server);
}

void hide()
{
    s_visible = false;
    if (s_overlay)
        s_overlay->set_visible(false);
    s_windows.clear();
    s_thumbnails.clear();
}

}
