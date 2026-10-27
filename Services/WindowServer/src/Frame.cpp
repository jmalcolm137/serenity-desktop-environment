/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Focus.h"
#include "Frame.h"
#include "Output.h"
#include "Pointer.h"
#include "Resources.h"
#include "WindowManager.h"
#include "WindowMenu.h"
#include "WindowSwitcher.h"
#include "XdgShell.h"

#include <LibCore/File.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/WindowTheme.h>
#include <WindowServer/WindowType.h>
#include <WindowServer/WindowMode.h>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Wlroots.h"

FrameHit hit_test_frame(Server& server, double x, double y)
{
    FrameHit hit;

    // Hit-test the windows in *stacking* order, topmost first.
    //
    // server.toplevels is in creation order and is never reordered, so scanning it
    // let a window that was created later and then occluded take the press -- and
    // when the point fell inside its hidden titlebar band the press began an
    // interactive *move* of a window the user cannot see ("trying to click a tab in
    // Firefox drags the Terminal that is running behind it"). The toplevel trees are
    // siblings under windows_tree and raising one moves it to the end of that list,
    // so walking the list backwards is the real z-order.
    wl_list* children = &server.windows_tree->children;
    for (wl_list* link = children->prev; link != children; link = link->prev) {
        struct wlr_scene_node* child_node;
        child_node = wl_container_of(link, child_node, link);
        if (child_node->type != WLR_SCENE_NODE_TREE)
            continue;
        struct wlr_scene_tree* tree;
        tree = wl_container_of(child_node, tree, node);

        Toplevel* window = nullptr;
        for (auto& candidate : server.toplevels) {
            if (candidate->scene_tree == tree) {
                window = candidate.ptr();
                break;
            }
        }
        // An X11 window's tree: x11_hit_test_frame() handles those.
        if (!window)
            continue;

        auto& toplevel = *window;
        // Hidden windows are not there to hit: minimised, or on another workspace
        // (their scene node is disabled).
        if (toplevel.minimized || !toplevel.scene_tree->node.enabled)
            continue;

        auto* node = &toplevel.scene_tree->node;
        Gfx::IntRect frame_rect { static_cast<int>(node->x), static_cast<int>(node->y), toplevel.frame_size.width(), toplevel.frame_size.height() };
        if (frame_rect.is_empty() || !frame_rect.contains({ static_cast<int>(x), static_cast<int>(y) }))
            continue;

        // The point is over this window and it is the topmost one, so the press
        // belongs to it -- and this is where the search must *stop*. A
        // client-side-decorated window (Firefox) has no compositor frame because it
        // draws its own titlebar and buttons, so there is no frame region to return
        // here; carrying on to the next window behind meant a press on Firefox's
        // titlebar was taken by a window underneath it, which is why clicking there
        // dragged or maximised the Terminal behind it.
        if (toplevel.client_side_decorations)
            return hit;

        hit.toplevel = &toplevel;
        hit.local = { static_cast<int>(x) - static_cast<int>(node->x), static_cast<int>(y) - static_cast<int>(node->y) };

        for (size_t button_index = 0; button_index < toplevel.button_rects.size(); ++button_index) {
            if (toplevel.button_rects[button_index].contains(hit.local)) {
                hit.region = FrameRegion::Button;
                hit.button_index = static_cast<int>(button_index);
                return hit;
            }
        }

        // The titlebar, expanded two pixels downwards (as in SerenityOS) so the
        // seam between it and the content is not a resize handle.
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

char const* resize_cursor_name(uint32_t edges)
{
    bool left = edges & WLR_EDGE_LEFT;
    bool right = edges & WLR_EDGE_RIGHT;
    bool top = edges & WLR_EDGE_TOP;
    bool bottom = edges & WLR_EDGE_BOTTOM;
    if ((left && top) || (right && bottom))
        return "nwse-resize";
    if ((right && top) || (left && bottom))
        return "nesw-resize";
    if (left || right)
        return "ew-resize";
    if (top || bottom)
        return "ns-resize";
    return "default";
}

void update_button_hover(Server& server, FrameHit const& hit)
{
    for (auto& toplevel : server.toplevels) {
        int hovered = -1;
        if (toplevel.ptr() == hit.toplevel && hit.region == FrameRegion::Button)
            hovered = hit.button_index;
        if (toplevel->hovered_button != hovered) {
            toplevel->hovered_button = hovered;
            toplevel_update_frame(*toplevel);
        }
    }
}

void update_button_pressed(Server& server, FrameHit const& hit)
{
    auto* toplevel = server.frame_press.toplevel;
    if (!toplevel)
        return;
    bool over = hit.toplevel == toplevel && hit.region == FrameRegion::Button && hit.button_index == server.frame_press.button_index;
    int pressed = over ? server.frame_press.button_index : -1;
    if (toplevel->pressed_button != pressed) {
        toplevel->pressed_button = pressed;
        toplevel_update_frame(*toplevel);
    }
}

static void set_scene_rect_color(wlr_scene_rect* rect, Gfx::Color color)
{
    float const alpha = color.alpha() / 255.0f;
    float const rgba[4] = {
        color.red() / 255.0f * alpha,
        color.green() / 255.0f * alpha,
        color.blue() / 255.0f * alpha,
        alpha,
    };
    wlr_scene_rect_set_color(rect, rgba);
}

void frame_buffers_set_visible(FrameBuffers& buffers, bool visible)
{
    if (buffers.tree)
        wlr_scene_node_set_enabled(&buffers.tree->node, visible);
}

void frame_buffers_update(FrameBuffers& buffers, wlr_scene_tree* parent, Gfx::Bitmap const& painted_frame,
    Gfx::IntSize frame_size, int frame_left, int frame_top, int frame_right, int frame_bottom, Gfx::Color background)
{
    if (frame_size.is_empty())
        return;

    if (!buffers.tree) {
        buffers.tree = wlr_scene_tree_create(parent);
        wlr_scene_node_lower_to_bottom(&buffers.tree->node);
    }
    wlr_scene_node_set_enabled(&buffers.tree->node, true);

    if (!buffers.background) {
        float const transparent[4] = { 0, 0, 0, 0 };
        buffers.background = wlr_scene_rect_create(buffers.tree, frame_size.width(), frame_size.height(), transparent);
    }
    wlr_scene_rect_set_size(buffers.background, frame_size.width(), frame_size.height());
    set_scene_rect_color(buffers.background, background);
    wlr_scene_node_set_position(&buffers.background->node, 0, 0);

    auto update_strip = [&](wlr_scene_buffer*& node, Gfx::IntRect rect, int x, int y) {
        if (!node)
            node = wlr_scene_buffer_create(buffers.tree, nullptr);
        if (rect.is_empty()) {
            wlr_scene_node_set_enabled(&node->node, false);
            return;
        }
        auto cropped = painted_frame.cropped(rect);
        if (cropped.is_error()) {
            wlr_scene_node_set_enabled(&node->node, false);
            return;
        }
        auto* buffer = bitmap_buffer_create(cropped.release_value());
        wlr_scene_buffer_set_buffer(node, buffer);
        wlr_buffer_drop(buffer);
        wlr_scene_node_set_position(&node->node, x, y);
        wlr_scene_buffer_set_dest_size(node, rect.width(), rect.height());
        use_nearest_filter(&node->node);
        wlr_scene_node_set_enabled(&node->node, true);
    };

    int const content_height = max(0, frame_size.height() - frame_top - frame_bottom);
    update_strip(buffers.top, { 0, 0, frame_size.width(), frame_top }, 0, 0);
    update_strip(buffers.bottom, { 0, frame_size.height() - frame_bottom, frame_size.width(), frame_bottom },
        0, frame_size.height() - frame_bottom);
    update_strip(buffers.left, { 0, frame_top, frame_left, content_height }, 0, frame_top);
    update_strip(buffers.right, { frame_size.width() - frame_right, frame_top, frame_right, content_height },
        frame_size.width() - frame_right, frame_top);
}

void toplevel_update_frame(Toplevel& toplevel)
{
    if (!g_palette)
        return;

    auto* xdg_toplevel = toplevel.xdg_toplevel;
    int content_width = xdg_toplevel->base->geometry.width;
    int content_height = xdg_toplevel->base->geometry.height;
    if (content_width <= 0 || content_height <= 0) {
        content_width = xdg_toplevel->base->surface->current.width;
        content_height = xdg_toplevel->base->surface->current.height;
    }
    if (content_width <= 0 || content_height <= 0)
        return;

    // A client that draws its own decorations (Firefox/GTK CSD) gets no
    // compositor frame: its content fills the whole window.
    if (toplevel.client_side_decorations) {
        toplevel.frame_left = 0;
        toplevel.frame_top = 0;
        toplevel.frame_right = 0;
        toplevel.frame_bottom = 0;
        toplevel.frame_size = { content_width, content_height };
        toplevel.button_rects.clear_with_capacity();
        toplevel.frame_cache.clear();
        frame_buffers_set_visible(toplevel.frame_buffers, false);
        wlr_scene_node_set_position(&toplevel.content_tree->node, 0, 0);
        wlr_scene_node_set_enabled(&toplevel.content_tree->node, true);
        return;
    }

    // A fullscreen window is frameless: the client content fills the output.
    if (toplevel.fullscreen) {
        toplevel.frame_left = 0;
        toplevel.frame_top = 0;
        toplevel.frame_right = 0;
        toplevel.frame_bottom = 0;
        toplevel.frame_size = { content_width, content_height };
        toplevel.button_rects.clear_with_capacity();
        toplevel.frame_cache.clear();
        frame_buffers_set_visible(toplevel.frame_buffers, false);
        wlr_scene_node_set_position(&toplevel.content_tree->node, 0, 0);
        wlr_scene_node_set_enabled(&toplevel.content_tree->node, true);
        return;
    }
    // The frame is visible again after leaving fullscreen.
    frame_buffers_set_visible(toplevel.frame_buffers, true);

    auto& theme = g_palette->window_theme();
    // The client may reserve a menubar strip above its surface. The client
    // presents that strip from a small subsurface at y = -inset; the scene tree
    // for the client surface is moved down by the same amount so the strip lands
    // at the frame's content top.
    int const menubar_inset = toplevel.menubar_inset;
    // A rolled-up window keeps only its frame: the content is hidden and the
    // frame is laid out as if the content had zero height.
    int frame_content_height = toplevel.rolled_up ? 0 : content_height + menubar_inset;
    Gfx::IntRect content_rect { 0, 0, content_width, frame_content_height };
    auto frame_rect = theme.frame_rect_for_window(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette, 0);
    auto frame_size = frame_rect.size();
    if (frame_size.is_empty())
        return;

    toplevel.frame_left = -frame_rect.x();
    toplevel.frame_top = -frame_rect.y();
    toplevel.frame_right = frame_size.width() - content_width - toplevel.frame_left;
    toplevel.frame_bottom = frame_size.height() - frame_content_height - toplevel.frame_top;
    toplevel.frame_size = frame_size;

    // The layout above is cheap and always kept current. Rendering the pixels is
    // not: a client commits new content every frame, but its decoration only
    // changes when one of these inputs does. Without this cache an idle window
    // re-rendered (and re-uploaded) its whole frame on every commit, which kept
    // the compositor's main thread busy even when nothing on screen changed.
    StringView const title_view = xdg_toplevel->title ? StringView { xdg_toplevel->title, strlen(xdg_toplevel->title) } : StringView { };
    bool const highlighted = &toplevel == WindowSwitcher::highlighted();
    bool const frame_dump_requested = getenv("WINDOWSERVER_DUMP_FRAME") != nullptr;
    auto const& frame_cache = toplevel.frame_cache;
    if (!frame_dump_requested && frame_cache.has_value()
        && frame_cache->content_width == content_width
        && frame_cache->content_height == content_height
        && frame_cache->active == toplevel.frame_active
        && frame_cache->maximized == toplevel.maximized
        && frame_cache->rolled_up == toplevel.rolled_up
        && frame_cache->highlighted == highlighted
        && frame_cache->hovered_button == toplevel.hovered_button
        && frame_cache->pressed_button == toplevel.pressed_button
        && frame_cache->menubar_inset == menubar_inset
        && frame_cache->icon == toplevel.icon.ptr()
        && frame_cache->scale == output_scale()
        && frame_cache->title == title_view)
        return;

    // Paint into a transient full-frame canvas so the theme's coordinates stay
    // exact. Only the decoration strips below are retained; the canvas is freed
    // when this function returns.
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, frame_size, output_scale()).release_value_but_fixme_should_propagate_errors();
    Gfx::Painter painter(*bitmap);
    painter.fill_rect(Gfx::IntRect { { }, frame_size }, g_palette->window());

    // Titlebar icon: the window's icon (set_icon), scaled to 16x16, else blank.
    static auto const blank_icon = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 16, 16 }).release_value_but_fixme_should_propagate_errors();
    RefPtr<Gfx::Bitmap> scaled_icon;
    if (toplevel.icon && toplevel.icon->width() > 0 && toplevel.icon->height() > 0) {
        if (auto scaled = toplevel.icon->scaled(16.0f / toplevel.icon->width(), 16.0f / toplevel.icon->height()); !scaled.is_error())
            scaled_icon = scaled.release_value();
    }
    Gfx::Bitmap const& icon = scaled_icon ? *scaled_icon : *blank_icon;
    auto buttons = theme.layout_buttons(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette, 3, false);
    toplevel.titlebar_icon_rect = theme.titlebar_icon_rect(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette);
    Gfx::IntRect leftmost_button = buttons.is_empty() ? Gfx::IntRect { } : buttons.last();
    auto state = toplevel.frame_active ? Gfx::WindowTheme::WindowState::Active : Gfx::WindowTheme::WindowState::Inactive;
    // While the Alt+Tab switcher is open, draw the selected window in the theme's
    // highlighted state (SerenityOS WindowManager::set_highlight_window).
    if (highlighted)
        state = Gfx::WindowTheme::WindowState::Highlighted;
    theme.paint_normal_frame(painter, state, Gfx::WindowTheme::WindowMode::Other, content_rect, title_view, icon, *g_palette, leftmost_button, 0, false);

    toplevel.button_rects.clear_with_capacity();
    float icon_opacity = toplevel.frame_active ? 1.0f : (g_palette->window_title_button_inactive_alpha() / 255.0f);
    for (size_t index = 0; index < buttons.size(); ++index) {
        // layout_buttons() returns rects in frame coordinates, which is also what
        // the frame bitmap and cursor hit-testing use.
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

    wlr_scene_node_set_position(&toplevel.content_tree->node, toplevel.frame_left, toplevel.frame_top + menubar_inset);
    wlr_scene_node_set_enabled(&toplevel.content_tree->node, !toplevel.rolled_up);

    if (auto const* dump_path = getenv("WINDOWSERVER_DUMP_FRAME"); dump_path && *dump_path) {
        if (auto encoded = Gfx::PNGWriter::encode(*bitmap); !encoded.is_error()) {
            if (auto file = Core::File::open(StringView { dump_path, strlen(dump_path) }, Core::File::OpenMode::Write); !file.is_error()) {
                if (auto write_result = file.value()->write_until_depleted(encoded.value()); write_result.is_error())
                    warnln("WindowServer: could not write the frame dump: {}", write_result.error());
            }
        }
    }

    // Retain only the decoration strips. The content area is a solid scene rect,
    // so a maximised window no longer keeps a full-window ARGB bitmap alive.
    frame_buffers_update(toplevel.frame_buffers, toplevel.scene_tree, *bitmap,
        frame_size, toplevel.frame_left, toplevel.frame_top, toplevel.frame_right, toplevel.frame_bottom,
        g_palette->window());

    // Remember what produced these pixels so identical commits skip the render.
    toplevel.frame_cache = FrameCache {
        .content_width = content_width,
        .content_height = content_height,
        .active = toplevel.frame_active,
        .maximized = toplevel.maximized,
        .rolled_up = toplevel.rolled_up,
        .highlighted = highlighted,
        .hovered_button = toplevel.hovered_button,
        .pressed_button = toplevel.pressed_button,
        .menubar_inset = menubar_inset,
        .icon = toplevel.icon.ptr(),
        .scale = output_scale(),
        .title = ByteString { title_view },
    };
}

// Decide and apply a toplevel's decoration mode. Wayland-first (P8): a client
// that does not ask for server-side decorations draws its own (Firefox/GTK CSD),
// so the compositor draws no frame; a client that requests server-side
// decorations (the LibGUI/Serenity apps do) gets the compositor frame. `set_mode`
// schedules a configure, so it only runs once the surface is initialized.
void apply_toplevel_decoration(Toplevel& toplevel)
{
    if (!toplevel.decoration || !toplevel.xdg_toplevel->base->initialized)
        return;

    bool client_side = toplevel.decoration->requested_mode != WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    // set_mode() schedules a configure, so only send it when we have not yet, or
    // when the client changed its mind — never on every commit (that would
    // configure-storm the client, which then repaints forever).
    if (toplevel.decoration_mode_applied && toplevel.client_side_decorations == client_side)
        return;

    toplevel.client_side_decorations = client_side;
    toplevel.decoration_mode_applied = true;
    if (toplevel.xdg_toplevel->base->surface->mapped)
        toplevel_update_frame(toplevel);
    wlr_xdg_toplevel_decoration_v1_set_mode(toplevel.decoration,
        client_side ? WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                    : WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

bool handle_frame_press(Server& server, uint32_t button, uint32_t time)
{
    auto hit = hit_test_frame(server, server.cursor->x, server.cursor->y);
    auto* toplevel = hit.toplevel;
    if (!toplevel)
        return false;

    // The window menu: right-clicking the titlebar, or left-clicking the window
    // icon at its left edge, opens it (as in SerenityOS).
    if (hit.region == FrameRegion::Titlebar) {
        bool icon_click = button == BTN_LEFT && !toplevel->titlebar_icon_rect.is_empty() && toplevel->titlebar_icon_rect.contains(hit.local);
        if (button == BTN_RIGHT || icon_click) {
            focus_toplevel(server, toplevel);
            server.frame_press.double_click_pending = false;
            open_window_menu(server, *toplevel, Gfx::IntPoint { static_cast<int>(server.cursor->x), static_cast<int>(server.cursor->y) });
            return true;
        }
    }

    if (button == BTN_LEFT) {
        if (hit.region == FrameRegion::Button) {
            // Track the button until release, like SerenityOS's Button: the
            // action fires only if the cursor is still over it on mouse up.
            server.frame_press.toplevel = toplevel;
            server.frame_press.button_index = hit.button_index;
            server.frame_press.double_click_pending = false;
            toplevel->hovered_button = hit.button_index;
            toplevel->pressed_button = hit.button_index;
            toplevel_update_frame(*toplevel);
            return true;
        }

        if (hit.region == FrameRegion::Titlebar) {
            focus_toplevel(server, toplevel);
            // Decide on release whether this is a double click; a first click
            // still starts a move so single-click dragging feels immediate.
            bool same_window = server.frame_press.last_titlebar_toplevel == toplevel;
            double dx = server.cursor->x - server.frame_press.last_titlebar_x;
            double dy = server.cursor->y - server.frame_press.last_titlebar_y;
            server.frame_press.double_click_pending = same_window
                && (time - server.frame_press.last_titlebar_time) < static_cast<uint32_t>(Config::input().mouse.double_click_speed)
                && (dx * dx + dy * dy) <= (6 * 6);
            server.frame_press.last_titlebar_toplevel = toplevel;
            server.frame_press.last_titlebar_time = time;
            server.frame_press.last_titlebar_x = server.cursor->x;
            server.frame_press.last_titlebar_y = server.cursor->y;
            toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
            return true;
        }

        if (hit.region == FrameRegion::Border) {
            focus_toplevel(server, toplevel);
            server.frame_press.double_click_pending = false;
            toplevel_begin_interactive(*toplevel, CursorMode::Resize, hit.edges);
            return true;
        }
    }

    return false;
}

void handle_frame_release(Server& server, uint32_t)
{
    if (server.frame_press.toplevel) {
        auto* toplevel = server.frame_press.toplevel;
        int index = server.frame_press.button_index;
        bool activate = toplevel->pressed_button == index;
        toplevel->pressed_button = -1;
        server.frame_press.toplevel = nullptr;
        server.frame_press.button_index = -1;
        toplevel_update_frame(*toplevel);
        if (activate) {
            if (index == 0)
                toplevel_close(*toplevel);
            else if (index == 1)
                toplevel_toggle_maximized(*toplevel);
            else if (index == 2)
                toplevel_toggle_minimized(*toplevel);
        }
        return;
    }

    if (server.frame_press.double_click_pending) {
        if (auto* toplevel = moving_toplevel(server))
            toplevel_toggle_maximized(*toplevel);
    }
    server.frame_press.double_click_pending = false;
    reset_cursor_mode(server);
}
