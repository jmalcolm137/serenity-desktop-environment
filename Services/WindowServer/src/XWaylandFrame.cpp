/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Frame.h"
#include "Output.h"
#include "Pointer.h"
#include "Resources.h"
#include "WindowSwitcher.h"
#include "XWayland.h"
#include "XWaylandFrame.h"

#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/WindowTheme.h>
#include <WindowServer/WindowType.h>
#include <WindowServer/WindowMode.h>

#include <linux/input-event-codes.h>
#include <string.h>
#include <unistd.h>

#include "Wlroots.h"

// --- X11 server-side decoration frames -------------------------------------

// Render the SerenityOS frame around an X11 window, unless the client draws its
// own decorations (Motif hints) or is an override-redirect window.
void x11_update_frame(X11Toplevel& toplevel)
{
    if (!g_palette || !toplevel.xsurface)
        return;
    auto* xsurface = toplevel.xsurface;
    int content_width = xsurface->width;
    int content_height = xsurface->height;
    if (content_width <= 0 || content_height <= 0)
        return;

    bool decorate = !toplevel.override_redirect && xsurface->decorations == WLR_XWAYLAND_SURFACE_DECORATIONS_ALL;
    if (!decorate) {
        toplevel.frame_decorated = false;
        toplevel.frame_left = toplevel.frame_top = toplevel.frame_right = toplevel.frame_bottom = 0;
        toplevel.frame_size = { content_width, content_height };
        toplevel.button_rects.clear_with_capacity();
        frame_buffers_set_visible(toplevel.frame_buffers, false);
        if (toplevel.scene)
            wlr_scene_node_set_position(&toplevel.scene->buffer->node, 0, 0);
        return;
    }

    auto& theme = g_palette->window_theme();
    Gfx::IntRect content_rect { 0, 0, content_width, content_height };
    auto frame_rect = theme.frame_rect_for_window(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette, 0);
    auto frame_size = frame_rect.size();
    if (frame_size.is_empty())
        return;
    toplevel.frame_decorated = true;
    toplevel.frame_left = -frame_rect.x();
    toplevel.frame_top = -frame_rect.y();
    toplevel.frame_right = frame_size.width() - content_width - toplevel.frame_left;
    toplevel.frame_bottom = frame_size.height() - content_height - toplevel.frame_top;
    toplevel.frame_size = frame_size;

    // Transient canvas for exact theme painting; only the decoration strips
    // created below are retained by the scene.
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, frame_size, output_scale()).release_value_but_fixme_should_propagate_errors();
    Gfx::Painter painter(*bitmap);
    painter.fill_rect(Gfx::IntRect { { }, frame_size }, g_palette->window());

    static auto const blank_icon = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 16, 16 }).release_value_but_fixme_should_propagate_errors();
    // The client's icon, which is the X11 default (a big "X") drawn in the titlebar
    // text colour until/unless the client supplies its own.
    Gfx::Bitmap const& icon = toplevel.icon ? *toplevel.icon : (g_default_window_icon ? *g_default_window_icon : *blank_icon);
    auto buttons = theme.layout_buttons(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette, 3, false);
    toplevel.titlebar_icon_rect = theme.titlebar_icon_rect(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette);
    Gfx::IntRect leftmost_button = buttons.is_empty() ? Gfx::IntRect { } : buttons.last();
    auto state = toplevel.frame_active ? Gfx::WindowTheme::WindowState::Active : Gfx::WindowTheme::WindowState::Inactive;
    // The Alt-Tab switcher draws the highlighted window in the theme's
    // Highlighted state (WindowSwitcher), like the xdg path in toplevel_update_frame.
    if (&toplevel == WindowSwitcher::highlighted_x11())
        state = Gfx::WindowTheme::WindowState::Highlighted;
    StringView title { xsurface->title ? xsurface->title : "", xsurface->title ? strlen(xsurface->title) : 0 };
    theme.paint_normal_frame(painter, state, Gfx::WindowTheme::WindowMode::Other, content_rect, title, icon, *g_palette, leftmost_button, 0, false);

    toplevel.button_rects.clear_with_capacity();
    float icon_opacity = toplevel.frame_active ? 1.0f : (g_palette->window_title_button_inactive_alpha() / 255.0f);
    for (size_t index = 0; index < buttons.size(); ++index) {
        auto button_rect = buttons[index];
        toplevel.button_rects.append(button_rect);
        bool pressed = toplevel.pressed_button == static_cast<int>(index);
        bool hovered = toplevel.hovered_button == static_cast<int>(index);
        if (!g_palette->title_buttons_icon_only())
            Gfx::StylePainter::paint_button(painter, button_rect, *g_palette, Gfx::ButtonStyle::Normal, pressed, hovered);
        RefPtr<Gfx::Bitmap> button_icon;
        if (index == 0)
            button_icon = g_frame_icons.close;
        else if (index == 1)
            button_icon = toplevel.maximized ? g_frame_icons.restore : g_frame_icons.maximize;
        else if (index == 2)
            button_icon = g_frame_icons.minimize;
        if (button_icon) {
            auto icon_location = button_rect.center().translated(-(button_icon->width() / 2), -(button_icon->height() / 2));
            if (pressed)
                icon_location.translate_by(1, 1);
            painter.blit(icon_location, *button_icon, button_icon->rect(), icon_opacity);
        }
    }

    // Keep only the titlebar/border strips; the content area is a scene rect.
    // The scene tree is the *frame* origin (as for xdg toplevels): the frame is
    // drawn at (0,0) and the client content is inset by the frame, so a window
    // that asks to be at (0,0) shows its titlebar on-screen.
    frame_buffers_update(toplevel.frame_buffers, toplevel.scene_tree, *bitmap,
        frame_size, toplevel.frame_left, toplevel.frame_top, toplevel.frame_right, toplevel.frame_bottom,
        g_palette->window());
    if (toplevel.scene)
        wlr_scene_node_set_position(&toplevel.scene->buffer->node, toplevel.frame_left, toplevel.frame_top);
    dbgln("WindowServer: X11 frame '{}' {}x{} (content {}x{})", title, frame_size.width(), frame_size.height(), content_width, content_height);
}

FrameHit x11_hit_test_frame(Server& server, double x, double y)
{
    FrameHit hit;

    // Same rule as hit_test_frame: stacking order, topmost first (the X11 windows
    // are siblings under windows_tree too, and server.x11_toplevels is creation
    // order).
    wl_list* children = &server.windows_tree->children;
    for (wl_list* link = children->prev; link != children; link = link->prev) {
        struct wlr_scene_node* child_node;
        child_node = wl_container_of(link, child_node, link);
        if (child_node->type != WLR_SCENE_NODE_TREE)
            continue;
        struct wlr_scene_tree* tree;
        tree = wl_container_of(child_node, tree, node);

        X11Toplevel* window = nullptr;
        for (auto& candidate : server.x11_toplevels) {
            if (candidate->scene_tree == tree) {
                window = candidate.ptr();
                break;
            }
        }
        if (!window)
            continue;

        auto& toplevel = *window;
        if (!toplevel.mapped || !toplevel.scene_tree->node.enabled)
            continue;

        auto* node = &toplevel.scene_tree->node;
        Gfx::IntRect frame_rect {
            static_cast<int>(node->x), static_cast<int>(node->y),
            toplevel.frame_size.width(), toplevel.frame_size.height()
        };
        if (frame_rect.is_empty() || !frame_rect.contains({ static_cast<int>(x), static_cast<int>(y) }))
            continue;

        // Topmost window, and the search stops here; an X11 window without a
        // compositor frame owns its own press (see hit_test_frame).
        if (!toplevel.frame_decorated)
            return hit;

        hit.x11_toplevel = &toplevel;
        hit.local = { static_cast<int>(x) - frame_rect.x(), static_cast<int>(y) - frame_rect.y() };
        for (size_t button_index = 0; button_index < toplevel.button_rects.size(); ++button_index) {
            if (toplevel.button_rects[button_index].contains(hit.local)) {
                hit.region = FrameRegion::Button;
                hit.button_index = static_cast<int>(button_index);
                return hit;
            }
        }
        Gfx::IntRect titlebar_rect {
            toplevel.frame_left, 0,
            toplevel.frame_size.width() - toplevel.frame_left - toplevel.frame_right,
            min(toplevel.frame_top + 2, toplevel.frame_size.height())
        };
        if (titlebar_rect.contains(hit.local)) {
            hit.region = FrameRegion::Titlebar;
            return hit;
        }
        constexpr int resize_border = 4;
        uint32_t edges = 0;
        if (hit.local.x() < resize_border)
            edges |= WLR_EDGE_LEFT;
        if (hit.local.x() >= toplevel.frame_size.width() - resize_border)
            edges |= WLR_EDGE_RIGHT;
        if (hit.local.y() < resize_border)
            edges |= WLR_EDGE_TOP;
        if (hit.local.y() >= toplevel.frame_size.height() - resize_border)
            edges |= WLR_EDGE_BOTTOM;
        if (edges) {
            hit.region = FrameRegion::Border;
            hit.edges = edges;
        }
        return hit;
    }
    return hit;
}

void x11_begin_interactive(X11Toplevel& toplevel, CursorMode mode, uint32_t edges)
{
    auto& server = *toplevel.server;
    int width = toplevel.xsurface->width;
    int height = toplevel.xsurface->height;
    if (mode == CursorMode::Move) {
        begin_grab(server, nullptr, &toplevel, mode, server.cursor->x - toplevel.scene_tree->node.x, server.cursor->y - toplevel.scene_tree->node.y);
        return;
    }
    // The scene tree is the frame origin; the frame edge is dragged.
    int const frame_w = toplevel.frame_size.width() > 0 ? toplevel.frame_size.width() : width;
    int const frame_h = toplevel.frame_size.height() > 0 ? toplevel.frame_size.height() : height;
    double border_x = toplevel.scene_tree->node.x + ((edges & WLR_EDGE_RIGHT) ? frame_w : 0);
    double border_y = toplevel.scene_tree->node.y + ((edges & WLR_EDGE_BOTTOM) ? frame_h : 0);
    wlr_box box { static_cast<int>(toplevel.scene_tree->node.x), static_cast<int>(toplevel.scene_tree->node.y), frame_w, frame_h };
    begin_grab(server, nullptr, &toplevel, mode, server.cursor->x - border_x, server.cursor->y - border_y, box, edges);
}

void forget_frame_press(Server& server, X11Toplevel& toplevel)
{
    if (server.frame_press.x11 == &toplevel)
        server.frame_press.x11 = nullptr;
}

static void x11_frame_button_action(X11Toplevel& toplevel, int button_index)
{
    switch (button_index) {
    case 0:
        wlr_xwayland_surface_close(toplevel.xsurface);
        break;
    case 1: {
        toplevel.maximized = !toplevel.maximized;
        if (toplevel.maximized) {
            toplevel.restore_size = { toplevel.xsurface->width, toplevel.xsurface->height };
            wlr_xwayland_surface_set_maximized(toplevel.xsurface, true, true);
            wlr_box box { };
            wlr_output_layout_get_box(toplevel.server->output_layout, nullptr, &box);
            wlr_scene_node_set_position(&toplevel.scene_tree->node, box.x, box.y);
            wlr_xwayland_surface_configure(toplevel.xsurface, box.x, box.y, box.width, box.height);
        } else {
            wlr_xwayland_surface_set_maximized(toplevel.xsurface, false, false);
            int w = toplevel.restore_size.width() > 0 ? toplevel.restore_size.width() : toplevel.xsurface->width;
            int h = toplevel.restore_size.height() > 0 ? toplevel.restore_size.height() : toplevel.xsurface->height;
            wlr_xwayland_surface_configure(toplevel.xsurface, static_cast<int>(toplevel.scene_tree->node.x), static_cast<int>(toplevel.scene_tree->node.y), w, h);
        }
        break;
    }
    case 2:
        wlr_xwayland_surface_set_minimized(toplevel.xsurface, true);
        break;
    default:
        break;
    }
}

bool handle_x11_frame_press(Server& server, uint32_t button)
{
    auto hit = x11_hit_test_frame(server, server.cursor->x, server.cursor->y);
    auto* toplevel = hit.x11_toplevel;
    if (!toplevel)
        return false;
    if (button == BTN_LEFT && hit.region == FrameRegion::Button) {
        server.frame_press.x11 = toplevel;
        server.frame_press.x11_button_index = hit.button_index;
        toplevel->hovered_button = hit.button_index;
        toplevel->pressed_button = hit.button_index;
        x11_update_frame(*toplevel);
        return true;
    }
    if (button == BTN_LEFT && hit.region == FrameRegion::Titlebar) {
        focus_x11_toplevel(server, toplevel);
        x11_begin_interactive(*toplevel, CursorMode::Move, 0);
        return true;
    }
    if (button == BTN_LEFT && hit.region == FrameRegion::Border) {
        focus_x11_toplevel(server, toplevel);
        x11_begin_interactive(*toplevel, CursorMode::Resize, hit.edges);
        return true;
    }
    return false;
}

void handle_x11_frame_release(Server& server, uint32_t button)
{
    auto* toplevel = server.frame_press.x11;
    if (!toplevel)
        return;
    server.frame_press.x11 = nullptr;
    int index = server.frame_press.x11_button_index;
    server.frame_press.x11_button_index = -1;
    toplevel->pressed_button = -1;
    if (button != BTN_LEFT) {
        x11_update_frame(*toplevel);
        return;
    }
    // Act only if the release is still over the same button.
    auto hit = x11_hit_test_frame(server, server.cursor->x, server.cursor->y);
    bool same = hit.x11_toplevel == toplevel && hit.region == FrameRegion::Button && hit.button_index == index;
    if (same)
        x11_frame_button_action(*toplevel, index);
    x11_update_frame(*toplevel);
}

// Update the hovered titlebar button for the X11 window under the cursor.
void update_x11_button_hover(Server& server, double x, double y)
{
    auto hit = x11_hit_test_frame(server, x, y);
    for (auto& toplevel : server.x11_toplevels) {
        int hovered = (toplevel.ptr() == hit.x11_toplevel && hit.region == FrameRegion::Button) ? hit.button_index : -1;
        if (toplevel->hovered_button != hovered) {
            toplevel->hovered_button = hovered;
            if (toplevel->frame_decorated)
                x11_update_frame(*toplevel);
        }
    }
}

// WindowManager host hooks for X11 windows (Taskbar focus/minimize).
void hook_focus_x11(Server& server, X11Toplevel* toplevel)
{
    if (!toplevel)
        return;
    if (toplevel->xsurface->minimized)
        wlr_xwayland_surface_set_minimized(toplevel->xsurface, false);
    focus_x11_toplevel(server, toplevel);
    // SerenityOS: activating a window closes any fragile (popup/menu) window.
    dismiss_fragile_windows(server, "window activated"sv);
}

void hook_set_minimized_x11(Server&, X11Toplevel& toplevel, bool minimized)
{
    // A blocking modal dialog cannot be minimized (SerenityOS Window::is_modal()).
    if (minimized && toplevel.modal)
        return;
    wlr_xwayland_surface_set_minimized(toplevel.xsurface, minimized);
}
