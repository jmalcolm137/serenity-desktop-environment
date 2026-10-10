/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WindowServer: the Serenity Desktop Environment's Wayland compositor.
 *
 * Phase 0 skeleton. It uses wlroots for the display/input plumbing and
 * SerenityOS userland (AK, LibCore, ...) for everything we write, following
 * SerenityOS conventions. Rendering chrome with LibGfx/LibGPU comes next.
 */

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>

#include "Config.h"
#include "Model.h"
#include "Overlays.h"
#include "WindowManager.h"

#include <LibCore/ArgsParser.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Notifier.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Rect.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/SystemTheme.h>
#include <LibGfx/WindowTheme.h>
#include <LibIPC/Connection.h>
#include <LibMain/Main.h>
#include <WindowServer/WindowType.h>

#include <ctime>
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

// Generated from WindowServer/protocol/serenity-window.xml.
#include "serenity-window-server-protocol.h"
}

namespace {

// Tell the client where its window actually is (the compositor places it).
static void toplevel_report_position(Toplevel& toplevel);

static void toplevel_update_frame(Toplevel&);
static bool handle_frame_press(Server&, uint32_t button, uint32_t time);
static void handle_frame_release(Server&, uint32_t button);
static void close_window_menu(Server&);
static void update_window_menu_hover(Server&, double, double);
static void window_menu_handle_press(Server&, uint32_t button);
static void window_menu_handle_release(Server&, uint32_t button);
static bool window_menu_handle_key(Server&, xkb_keysym_t);
static void arrange_layers(Server&, wlr_output*);
static Output* output_for(Server&, wlr_output*);
static void output_manager_send_configuration(Server&);
static i32 g_next_wm_id { 1 };
static Gfx::Palette* g_palette { nullptr };

struct FrameIcons {
    RefPtr<Gfx::Bitmap> close;
    RefPtr<Gfx::Bitmap> minimize;
    RefPtr<Gfx::Bitmap> maximize;
    RefPtr<Gfx::Bitmap> restore;
    RefPtr<Gfx::Bitmap> move;
    RefPtr<Gfx::Bitmap> pin;
};
static FrameIcons g_frame_icons;

// Desktop background: a wallpaper image (optional) drawn over the theme's
// background colour. The Display Settings app writes the choice (image, mode and
// colour) to a shared state directory; we load it at startup and watch it.
static RefPtr<Gfx::Bitmap> g_wallpaper;
static ByteString g_wallpaper_mode { "center" };
static Optional<Gfx::Color> g_background_color;


// Raw-pixel wallpaper state (no PNG encode: that costs ~800ms and would stall
// the applying application). Must match WallpaperHeader in the client shim
// (libgui-wayland/src/WindowServerConnection.cpp).
struct WallpaperHeader {
    u32 magic;
    u32 format;
    u32 width;
    u32 height;
    u32 pitch;
};
static constexpr u32 wallpaper_magic = 0x53445750; // "SDWP"

static void write_sde_wallpaper(RefPtr<Gfx::Bitmap> const& bitmap)
{
    if (!bitmap) {
        Config::write_state("wallpaper.raw"sv, {});
        return;
    }
    auto const pixels = static_cast<size_t>(bitmap->pitch()) * bitmap->height();
    WallpaperHeader header {
        wallpaper_magic,
        static_cast<u32>(bitmap->format()),
        static_cast<u32>(bitmap->width()),
        static_cast<u32>(bitmap->height()),
        static_cast<u32>(bitmap->pitch()),
    };
    auto buffer = ByteBuffer::create_uninitialized(sizeof(header) + pixels).release_value_but_fixme_should_propagate_errors();
    memcpy(buffer.data(), &header, sizeof(header));
    memcpy(buffer.data() + sizeof(header), bitmap->scanline(0), pixels);
    Config::write_state("wallpaper.raw"sv, buffer);
}

static RefPtr<Gfx::Bitmap> read_sde_wallpaper()
{
    auto raw = Config::read_state_bytes("wallpaper.raw"sv);
    if (raw.size() <= sizeof(WallpaperHeader))
        return nullptr;
    WallpaperHeader header {};
    memcpy(&header, raw.data(), sizeof(header));
    if (header.magic != wallpaper_magic || header.width == 0 || header.height == 0 || header.pitch == 0)
        return nullptr;
    auto bitmap = Gfx::Bitmap::create(static_cast<Gfx::BitmapFormat>(header.format), { static_cast<int>(header.width), static_cast<int>(header.height) });
    if (bitmap.is_error())
        return nullptr;
    auto& created = *bitmap.value();
    auto const row_bytes = min(static_cast<size_t>(header.pitch), static_cast<size_t>(created.pitch()));
    for (int y = 0; y < created.height(); ++y)
        memcpy(created.scanline(y), raw.data() + sizeof(header) + static_cast<size_t>(y) * header.pitch, row_bytes);
    return bitmap.release_value();
}

static RefPtr<Gfx::Bitmap> load_icon(ByteString const& resource_root, StringView name, StringView fallback)
{
    auto path = ByteString::formatted("{}/icons/16x16/{}.png", resource_root, name);
    if (auto bitmap = Gfx::Bitmap::load_from_file(path); !bitmap.is_error())
        return bitmap.release_value();
    if (!fallback.is_empty()) {
        auto fallback_path = ByteString::formatted("{}/icons/16x16/{}.png", resource_root, fallback);
        if (auto bitmap = Gfx::Bitmap::load_from_file(fallback_path); !bitmap.is_error())
            return bitmap.release_value();
    }
    return nullptr;
}


template<typename T>
static void remove_owned(Vector<NonnullOwnPtr<T>>& list, T* object)
{
    for (size_t index = 0; index < list.size(); ++index) {
        if (list[index].ptr() == object) {
            list.remove(index);
            return;
        }
    }
}

static Toplevel* toplevel_from_handle(wlr_foreign_toplevel_handle_v1* handle)
{
    return static_cast<Toplevel*>(handle->data);
}

void focus_toplevel(Server& server, Toplevel* toplevel)
{
    if (!toplevel)
        return;

    wlr_surface* previous_surface = server.seat->keyboard_state.focused_surface;
    wlr_surface* surface = toplevel->xdg_toplevel->base->surface;
    if (previous_surface == surface)
        return;

    if (previous_surface) {
        if (auto* previous = wlr_xdg_toplevel_try_from_wlr_surface(previous_surface)) {
            wlr_xdg_toplevel_set_activated(previous, false);
            if (auto* content_tree = static_cast<wlr_scene_tree*>(previous->base->data)) {
                if (auto* frame_tree = content_tree->node.parent) {
                    if (auto* previous_toplevel = static_cast<Toplevel*>(frame_tree->node.data)) {
                        previous_toplevel->frame_active = false;
                        toplevel_update_frame(*previous_toplevel);
                    }
                }
            }
        }
    }

    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);

    // Always-on-top windows stay above normal ones.
    for (auto& candidate : server.toplevels) {
        if (candidate->always_on_top && !candidate->minimized)
            wlr_scene_node_raise_to_top(&candidate->scene_tree->node);
    }

    wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
    toplevel->frame_active = true;
    toplevel_update_frame(*toplevel);
    if (toplevel->foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_activated(toplevel->foreign_handle, true);

    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

// --- keyboard -------------------------------------------------------------

static void keyboard_modifiers(wl_listener* listener, void*)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, modifiers);
    wlr_seat_set_keyboard(keyboard->server->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_modifiers(keyboard->server->seat, &keyboard->keyboard->modifiers);
}

static bool handle_keybinding(Server& server, xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Escape:
        wl_display_terminate(server.display);
        return true;
    default:
        return false;
    }
}

static void keyboard_key(wl_listener* listener, void* data)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, key);
    auto& server = *keyboard->server;
    auto* event = static_cast<wlr_keyboard_key_event*>(data);

    uint32_t keycode = event->keycode + 8;
    xkb_keysym_t const* syms = nullptr;
    int syms_count = xkb_state_key_get_syms(keyboard->keyboard->xkb_state, keycode, &syms);

    bool handled = false;
    uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
    if (server.window_menu.open) {
        // The open window menu grabs the keyboard.
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < syms_count; ++i)
                window_menu_handle_key(server, syms[i]);
        }
        return;
    }
    if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < syms_count; ++i)
            handled |= handle_keybinding(server, syms[i]);
    }

    if (!handled) {
        wlr_seat_set_keyboard(server.seat, keyboard->keyboard);
        wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
    }
}

static void keyboard_destroy(wl_listener* listener, void*)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, destroy);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    remove_owned(keyboard->server->keyboards, keyboard);
}

// --- cursor / pointer -----------------------------------------------------

static Toplevel* toplevel_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy)
{
    wlr_scene_node* node = wlr_scene_node_at(&server.scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return nullptr;

    auto* scene_buffer = wlr_scene_buffer_from_node(node);
    auto* scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);
    if (!scene_surface)
        return nullptr;

    *surface = scene_surface->surface;
    // Walk up to the scene tree that belongs to an xdg toplevel. We can't just
    // stop at the first tree with node.data set: layer surfaces (the Taskbar)
    // store their own object there, so match against the known toplevel trees.
    wlr_scene_tree* tree = node->parent;
    while (tree) {
        for (auto& toplevel : server.toplevels) {
            if (toplevel->scene_tree == tree)
                return toplevel.ptr();
        }
        tree = tree->node.parent;
    }
    return nullptr;
}

static void reset_cursor_mode(Server& server)
{
    server.cursor_mode = CursorMode::Passthrough;
    server.grabbed_toplevel = nullptr;
}

static void process_cursor_move(Server& server)
{
    auto* toplevel = server.grabbed_toplevel;
    if (!toplevel)
        return;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, server.cursor->x - server.grab_x, server.cursor->y - server.grab_y);
}

static void process_cursor_resize(Server& server)
{
    auto* toplevel = server.grabbed_toplevel;
    if (!toplevel)
        return;

    double border_x = server.cursor->x - server.grab_x;
    double border_y = server.cursor->y - server.grab_y;
    int new_left = server.grab_box.x;
    int new_right = server.grab_box.x + server.grab_box.width;
    int new_top = server.grab_box.y;
    int new_bottom = server.grab_box.y + server.grab_box.height;

    if (server.resize_edges & WLR_EDGE_TOP) {
        new_top = border_y;
        if (new_top >= new_bottom)
            new_top = new_bottom - 1;
    } else if (server.resize_edges & WLR_EDGE_BOTTOM) {
        new_bottom = border_y;
        if (new_bottom <= new_top)
            new_bottom = new_top + 1;
    }
    if (server.resize_edges & WLR_EDGE_LEFT) {
        new_left = border_x;
        if (new_left >= new_right)
            new_left = new_right - 1;
    } else if (server.resize_edges & WLR_EDGE_RIGHT) {
        new_right = border_x;
        if (new_right <= new_left)
            new_right = new_left + 1;
    }

    auto* geometry = &toplevel->xdg_toplevel->base->geometry;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left - geometry->x, new_top - geometry->y);
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, new_right - new_left, new_bottom - new_top);
}

enum class FrameRegion {
    None,
    Button,
    Titlebar,
    Border,
};

// Where the cursor is within a window's server-side frame.
struct FrameHit {
    Toplevel* toplevel { nullptr };
    FrameRegion region { FrameRegion::None };
    int button_index { -1 };
    uint32_t edges { 0 };
    Gfx::IntPoint local;
};

static FrameHit hit_test_frame(Server& server, double x, double y)
{
    FrameHit hit;
    for (size_t index = server.toplevels.size(); index-- > 0;) {
        auto& toplevel = *server.toplevels[index];
        if (toplevel.minimized)
            continue;

        auto* node = &toplevel.scene_tree->node;
        Gfx::IntRect frame_rect { static_cast<int>(node->x), static_cast<int>(node->y), toplevel.frame_size.width(), toplevel.frame_size.height() };
        if (frame_rect.is_empty() || !frame_rect.contains({ static_cast<int>(x), static_cast<int>(y) }))
            continue;

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

static char const* resize_cursor_name(uint32_t edges)
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

static void update_button_hover(Server& server, FrameHit const& hit)
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

static void update_button_pressed(Server& server, FrameHit const& hit)
{
    auto* toplevel = server.pressed_button_toplevel;
    if (!toplevel)
        return;
    bool over = hit.toplevel == toplevel && hit.region == FrameRegion::Button && hit.button_index == server.pressed_button_index;
    int pressed = over ? server.pressed_button_index : -1;
    if (toplevel->pressed_button != pressed) {
        toplevel->pressed_button = pressed;
        toplevel_update_frame(*toplevel);
    }
}

static void process_cursor_motion(Server& server, uint32_t time)
{
    if (server.window_menu.open) {
        update_window_menu_hover(server, server.cursor->x, server.cursor->y);
        wlr_seat_pointer_clear_focus(server.seat);
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
        return;
    }

    if (server.cursor_mode == CursorMode::Move) {
        process_cursor_move(server);
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "grabbing");
        return;
    }
    if (server.cursor_mode == CursorMode::Resize) {
        process_cursor_resize(server);
        return;
    }

    // Implicit grab: a client button is down, so keep delivering motion to the
    // surface that received the press until it is released.
    if (server.pointer_grab_surface) {
        if (server.seat->pointer_state.focused_surface != server.pointer_grab_surface) {
            // The grabbed surface went away (e.g. the client disconnected).
            server.pointer_grab_surface = nullptr;
            server.pointer_buttons_down = 0;
        } else {
            wlr_seat_pointer_notify_motion(server.seat, time,
                server.cursor->x - server.pointer_grab_origin_x,
                server.cursor->y - server.pointer_grab_origin_y);
            return;
        }
    }

    auto hit = hit_test_frame(server, server.cursor->x, server.cursor->y);
    update_button_hover(server, hit);
    update_button_pressed(server, hit);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    if (hit.region == FrameRegion::None)
        toplevel_at(server, server.cursor->x, server.cursor->y, &surface, &sx, &sy);

    if (surface) {
        if (server.seat->pointer_state.focused_surface != surface)
            wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
        wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(server.seat);
    }

    // Set the cursor for our chrome last: clearing client focus above resets it
    // to the default arrow, so the resize cursor must come after.
    if (hit.region != FrameRegion::None)
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, hit.region == FrameRegion::Border ? resize_cursor_name(hit.edges) : "default");
}

static void cursor_motion(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_motion);
    auto* event = static_cast<wlr_pointer_motion_event*>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    process_cursor_motion(*server, event->time_msec);
}

static void cursor_motion_absolute(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_motion_absolute);
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    process_cursor_motion(*server, event->time_msec);
}

// Client menus opened from layer surfaces (the Taskbar/Desktop) are overlay layer
// surfaces with no input grab, so wlroots will not route an outside press to
// them and the client cannot dismiss them. On a press that lands on another
// client's surface, ask the menu owner to close its menus.
static void maybe_dismiss_layer_menus(Server& server)
{
    LayerSurface* menu_layer = nullptr;
    for (auto& layer : server.layer_surfaces) {
        if (!layer->is_menu || !layer->layer_surface || !layer->layer_surface->surface)
            continue;
        if (!layer->layer_surface->surface->mapped)
            continue;
        menu_layer = layer.ptr();
        break;
    }
    if (!menu_layer)
        return;

    auto* owner_client = wl_resource_get_client(menu_layer->layer_surface->resource);
    auto* focused = server.seat->pointer_state.focused_surface;
    // A press on one of the owner's own surfaces (its button, or the menu itself)
    // is handled by the client; only presses elsewhere need dismissing.
    if (focused && wl_resource_get_client(focused->resource) == owner_client)
        return;

    for (auto* manager : server.serenity_window_managers) {
        if (wl_resource_get_client(manager) == owner_client)
            serenity_window_manager_send_dismiss_menus(manager);
    }
}

static void cursor_button(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_button);
    auto* event = static_cast<wlr_pointer_button_event*>(data);

    if (server->window_menu.open) {
        if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
            window_menu_handle_press(*server, event->button);
        } else {
            window_menu_handle_release(*server, event->button);
            // The client saw the press that opened the menu, so let it see the
            // release too and keep its button state (and our implicit grab)
            // balanced. The menu may have swallowed any number of presses; a
            // stray release is harmless to the client.
            wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
            server->pointer_buttons_down = 0;
            server->pointer_grab_surface = nullptr;
        }
        return;
    }

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        if (handle_frame_press(*server, event->button, event->time_msec)) {
            // Opening the window menu is not an interactive frame grab.
            if (!server->window_menu.open)
                server->frame_grab = true;
            return;
        }
    } else if (server->frame_grab) {
        server->frame_grab = false;
        handle_frame_release(*server, event->button);
        return;
    }

    // Dismiss client menus (opened from layer surfaces) on an outside press.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
        maybe_dismiss_layer_menus(*server);

    auto serial = wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // A drag has ended: tell the client where its window ended up.
        if (server->grabbed_toplevel)
            toplevel_report_position(*server->grabbed_toplevel);
        if (server->pointer_buttons_down > 0)
            --server->pointer_buttons_down;
        if (server->pointer_buttons_down == 0)
            server->pointer_grab_surface = nullptr;
        reset_cursor_mode(*server);
    } else {
        double sx = 0;
        double sy = 0;
        wlr_surface* surface = nullptr;
        focus_toplevel(*server, toplevel_at(*server, server->cursor->x, server->cursor->y, &surface, &sx, &sy));
        // The seat accepted the press: remember the surface so the release finds
        // its way back to the same client even if the cursor moves away.
        if (serial && surface) {
            server->pointer_grab_surface = surface;
            server->pointer_grab_origin_x = server->cursor->x - sx;
            server->pointer_grab_origin_y = server->cursor->y - sy;
            ++server->pointer_buttons_down;
        }
    }
}

static void cursor_axis(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_axis);
    auto* event = static_cast<wlr_pointer_axis_event*>(data);
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, event->delta, event->delta_discrete, event->source, event->relative_direction);
}

static void cursor_frame(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_frame);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_request_cursor);
    auto* event = static_cast<wlr_seat_pointer_request_set_cursor_event*>(data);
    if (server->seat->pointer_state.focused_client == event->seat_client)
        wlr_cursor_set_surface(server->cursor, event->surface, event->hotspot_x, event->hotspot_y);
}

static void seat_pointer_focus_change(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_pointer_focus_change);
    auto* event = static_cast<wlr_seat_pointer_focus_change_event*>(data);
    if (!event->new_surface)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, "default");
}

static void seat_request_set_selection(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_request_set_selection);
    auto* event = static_cast<wlr_seat_request_set_selection_event*>(data);
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void server_new_input(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_input);
    auto* device = static_cast<wlr_input_device*>(data);

    switch (device->type) {
    case WLR_INPUT_DEVICE_KEYBOARD: {
        auto keyboard = make<Keyboard>();
        keyboard->server = server;
        keyboard->keyboard = wlr_keyboard_from_input_device(device);

        auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        auto* keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
        wlr_keyboard_set_keymap(keyboard->keyboard, keymap);
        xkb_keymap_unref(keymap);
        xkb_context_unref(context);
        wlr_keyboard_set_repeat_info(keyboard->keyboard, 25, 600);

        keyboard->modifiers.notify = keyboard_modifiers;
        wl_signal_add(&keyboard->keyboard->events.modifiers, &keyboard->modifiers);
        keyboard->key.notify = keyboard_key;
        wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
        keyboard->destroy.notify = keyboard_destroy;
        wl_signal_add(&device->events.destroy, &keyboard->destroy);

        wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
        server->keyboards.append(move(keyboard));
        break;
    }
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    default:
        break;
    }

    uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
    if (!server->keyboards.is_empty())
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    wlr_seat_set_capabilities(server->seat, capabilities);
}

// Virtual input (wlr virtual-keyboard / virtual-pointer). Both embed a real
// wlr_input_device, so route them through the same setup as physical input - this
// is what lets the headless harness script keyboard and pointer events.
static void server_new_virtual_keyboard(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_virtual_keyboard);
    auto* keyboard = static_cast<wlr_virtual_keyboard_v1*>(data);
    dbgln("WindowServer: virtual keyboard connected");
    server_new_input(&server->new_input, &keyboard->keyboard.base);
}

static void server_new_virtual_pointer(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_virtual_pointer);
    auto* event = static_cast<wlr_virtual_pointer_v1_new_pointer_event*>(data);
    dbgln("WindowServer: virtual pointer connected");
    server_new_input(&server->new_input, &event->new_pointer->pointer.base);
}

// --- outputs --------------------------------------------------------------

// Compose the desktop background for an output: the theme's background colour,
// optionally overlaid with a wallpaper image. Rebuilt only when the output size
// changes.
static void update_desktop_background(Server& server, Output& output)
{
    if (!g_palette)
        return;
    int width = output.output->width;
    int height = output.output->height;
    if (width <= 0 || height <= 0)
        return;

    Gfx::IntSize size { width, height };
    if (output.background && output.background_size == size)
        return;

    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, size).release_value_but_fixme_should_propagate_errors();
    Gfx::Painter painter(*bitmap);
    painter.fill_rect(bitmap->rect(), g_background_color.value_or(g_palette->desktop_background()));

    if (g_wallpaper) {
        auto const& wallpaper = *g_wallpaper;
        if (g_wallpaper_mode == "tile"sv) {
            painter.draw_tiled_bitmap(bitmap->rect(), wallpaper);
        } else if (g_wallpaper_mode == "stretch"sv) {
            painter.draw_scaled_bitmap(bitmap->rect(), wallpaper, wallpaper.rect());
        } else if (g_wallpaper_mode == "fill"sv && wallpaper.width() > 0 && wallpaper.height() > 0) {
            double const scale = max(static_cast<double>(width) / wallpaper.width(), static_cast<double>(height) / wallpaper.height());
            int const src_width = min(wallpaper.width(), static_cast<int>(width / scale));
            int const src_height = min(wallpaper.height(), static_cast<int>(height / scale));
            Gfx::IntRect const src { (wallpaper.width() - src_width) / 2, (wallpaper.height() - src_height) / 2, src_width, src_height };
            painter.draw_scaled_bitmap(bitmap->rect(), wallpaper, src);
        } else {
            // Center (the SerenityOS default).
            Gfx::IntPoint offset { (width - wallpaper.width()) / 2, (height - wallpaper.height()) / 2 };
            painter.blit(offset, wallpaper, wallpaper.rect());
        }
    }

    if (auto const* dump_path = getenv("WINDOWSERVER_DUMP_BACKGROUND"); dump_path && *dump_path) {
        if (auto encoded = Gfx::PNGWriter::encode(*bitmap); !encoded.is_error()) {
            if (auto file = Core::File::open(StringView { dump_path, strlen(dump_path) }, Core::File::OpenMode::Write); !file.is_error())
                (void)file.value()->write_until_depleted(encoded.value());
        }
    }

    auto* buffer = bitmap_buffer_create(move(bitmap));
    if (!output.background) {
        output.background = wlr_scene_buffer_create(&server.scene->tree, buffer);
        wlr_scene_node_lower_to_bottom(&output.background->node);
    } else {
        wlr_scene_buffer_set_buffer(output.background, buffer);
    }
    wlr_buffer_drop(buffer);
    output.background_size = size;

    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, output.output, &box);
    wlr_scene_node_set_position(&output.background->node, box.x, box.y);
}

// Re-read the persisted wallpaper state into the globals (no repaint yet).
static void load_wallpaper_state()
{
    g_wallpaper = read_sde_wallpaper();

    g_wallpaper_mode = "center";
    if (auto mode = Config::read_state("wallpaper.mode"sv); !mode.is_empty())
        g_wallpaper_mode = mode.to_lowercase();

    g_background_color = {};
    if (auto color = Config::read_state("wallpaper.color"sv); !color.is_empty())
        g_background_color = Gfx::Color::from_string(color);
}

// Re-read the state and repaint every output (used on a live change).
static void apply_wallpaper_from_state(Server& server)
{
    load_wallpaper_state();
    for (auto& output : server.outputs) {
        output->background_size = { };
        update_desktop_background(server, *output);
    }
    dbgln("WindowServer: wallpaper updated (wallpaper={}, mode={}, color={})",
        g_wallpaper ? ByteString::formatted("{}x{}", g_wallpaper->width(), g_wallpaper->height()) : ByteString { "none" },
        g_wallpaper_mode,
        g_background_color.has_value() ? g_background_color->to_string() : "theme"_string);
}

// On first run, seed the state directory from SDE_WALLPAPER / SDE_WALLPAPER_MODE,
// then load it. Afterwards the app-managed state is authoritative, so a chosen
// wallpaper (and "None") persist across restarts.
static void initialize_wallpaper(ByteString const& resource_root)
{
    (void)Core::Directory::create(Config::state_directory(), Core::Directory::CreateDirectories::Yes);

    if (Core::File::open(Config::state_file("wallpaper.mode"sv), Core::File::OpenMode::Read).is_error()) {
        ByteString mode { "center" };
        if (auto const* env_mode = getenv("SDE_WALLPAPER_MODE"); env_mode && *env_mode)
            mode = env_mode;
        ByteString normalized_mode = mode.to_lowercase();
        Config::write_state("wallpaper.mode"sv, normalized_mode.bytes());
        Config::write_state("wallpaper.color"sv, {});
        // Ensure the image file exists (empty == no wallpaper) so its content
        // watch attaches; overwrite it if the environment names a wallpaper.
        Config::write_state("wallpaper.raw"sv, {});
        if (auto const* env_wallpaper = getenv("SDE_WALLPAPER"); env_wallpaper && *env_wallpaper) {
            ByteString path { env_wallpaper };
            if (!path.starts_with('/'))
                path = ByteString::formatted("{}/wallpapers/{}", resource_root, path);
            if (auto bitmap = Gfx::Bitmap::load_from_file(path); !bitmap.is_error()) {
                write_sde_wallpaper(bitmap.value());
                dbgln("WindowServer: seeded wallpaper from '{}' ({}x{})", path, bitmap.value()->width(), bitmap.value()->height());
            } else {
                warnln("WindowServer: could not load the seed wallpaper '{}': {}", path, bitmap.error());
            }
        }
    }

    load_wallpaper_state();
}

static void watch_wallpaper_state(Server& server)
{
    static RefPtr<Core::FileWatcher> watcher;
    static RefPtr<Core::Timer> coalesce;
    // A single change produces several inotify events (file truncate + write,
    // plus the mode/colour files); coalesce them into one reload.
    coalesce = Core::Timer::create_single_shot(30, [&server] { apply_wallpaper_from_state(server); });
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto directory = Config::state_directory();
    watcher->on_change = [&server, directory](Core::FileWatcherEvent const& event) {
        if (!event.event_path.starts_with(directory))
            return;
        coalesce->restart();
    };
    (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated | Core::FileWatcherEvent::Type::ChildDeleted);
    for (auto name : { "wallpaper.raw"sv, "wallpaper.mode"sv, "wallpaper.color"sv })
        (void)watcher->add_watch(Config::state_file(name), Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified | Core::FileWatcherEvent::Type::Deleted);
}

static void output_frame(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, frame);
    update_desktop_background(*output->server, *output);
    auto* scene_output = wlr_scene_get_scene_output(output->server->scene, output->output);
    if (!scene_output)
        return;
    wlr_scene_output_commit(scene_output, nullptr);
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(wl_listener* listener, void* data)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, request_state);
    auto* event = static_cast<wlr_output_event_request_state*>(data);
    wlr_output_commit_state(output->output, event->state);
}

static void output_destroy(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, destroy);
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    if (output->background)
        wlr_scene_node_destroy(&output->background->node);
    remove_owned(output->server->outputs, output);
}

static void server_new_output(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_output);
    auto* wlr_output = static_cast<struct wlr_output*>(data);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (auto* mode = wlr_output_preferred_mode(wlr_output))
        wlr_output_state_set_mode(&state, mode);
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    auto output = make<Output>();
    output->server = server;
    output->output = wlr_output;
    output->frame.notify = output_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);
    output->request_state.notify = output_request_state;
    wl_signal_add(&wlr_output->events.request_state, &output->request_state);
    output->destroy.notify = output_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    auto* layout_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    auto* scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);

    if (!server->primary_output)
        server->primary_output = wlr_output;

    dbgln("WindowServer: output '{}' {}x{}", wlr_output->name ? wlr_output->name : "?", wlr_output->width, wlr_output->height);
    server->outputs.append(move(output));
    update_desktop_background(*server, *server->outputs.last());
    arrange_layers(*server, wlr_output);
    output_manager_send_configuration(*server);
}

// --- xdg toplevels / popups ----------------------------------------------

static void toplevel_report_position(Toplevel& toplevel)
{
    if (toplevel.chrome)
        serenity_toplevel_send_position(toplevel.chrome, toplevel.scene_tree->node.x, toplevel.scene_tree->node.y);
}

static void toplevel_update_foreign(Toplevel& toplevel)
{
    if (!toplevel.foreign_handle)
        return;
    wlr_foreign_toplevel_handle_v1_set_title(toplevel.foreign_handle, toplevel.xdg_toplevel->title ? toplevel.xdg_toplevel->title : "");
    wlr_foreign_toplevel_handle_v1_set_app_id(toplevel.foreign_handle, toplevel.xdg_toplevel->app_id ? toplevel.xdg_toplevel->app_id : "");
}

static void toplevel_update_frame(Toplevel& toplevel)
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

    // A fullscreen window is frameless: the client content fills the output.
    if (toplevel.fullscreen) {
        toplevel.frame_left = 0;
        toplevel.frame_top = 0;
        toplevel.frame_right = 0;
        toplevel.frame_bottom = 0;
        toplevel.frame_size = { content_width, content_height };
        toplevel.button_rects.clear_with_capacity();
        if (toplevel.frame_buffer)
            wlr_scene_node_set_enabled(&toplevel.frame_buffer->node, false);
        wlr_scene_node_set_position(&toplevel.content_tree->node, 0, 0);
        wlr_scene_node_set_enabled(&toplevel.content_tree->node, true);
        return;
    }
    // The frame is visible again after leaving fullscreen.
    if (toplevel.frame_buffer)
        wlr_scene_node_set_enabled(&toplevel.frame_buffer->node, true);

    auto& theme = g_palette->window_theme();
    // A rolled-up window keeps only its frame: the content is hidden and the
    // frame is laid out as if the content had zero height.
    int frame_content_height = toplevel.rolled_up ? 0 : content_height;
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

    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, frame_size).release_value_but_fixme_should_propagate_errors();
    Gfx::Painter painter(*bitmap);
    painter.fill_rect(Gfx::IntRect { { }, frame_size }, g_palette->window());

    static auto const icon = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { 16, 16 }).release_value_but_fixme_should_propagate_errors();
    auto buttons = theme.layout_buttons(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette, 3, false);
    toplevel.titlebar_icon_rect = theme.titlebar_icon_rect(Gfx::WindowTheme::WindowType::Normal, Gfx::WindowTheme::WindowMode::Other, content_rect, *g_palette);
    Gfx::IntRect leftmost_button = buttons.is_empty() ? Gfx::IntRect { } : buttons.last();
    auto state = toplevel.frame_active ? Gfx::WindowTheme::WindowState::Active : Gfx::WindowTheme::WindowState::Inactive;
    StringView title;
    if (xdg_toplevel->title)
        title = StringView { xdg_toplevel->title, strlen(xdg_toplevel->title) };
    theme.paint_normal_frame(painter, state, Gfx::WindowTheme::WindowMode::Other, content_rect, title, *icon, *g_palette, leftmost_button, 0, false);

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

    wlr_scene_node_set_position(&toplevel.content_tree->node, toplevel.frame_left, toplevel.frame_top);
    wlr_scene_node_set_enabled(&toplevel.content_tree->node, !toplevel.rolled_up);

    if (auto const* dump_path = getenv("WINDOWSERVER_DUMP_FRAME"); dump_path && *dump_path) {
        if (auto encoded = Gfx::PNGWriter::encode(*bitmap); !encoded.is_error()) {
            if (auto file = Core::File::open(StringView { dump_path, strlen(dump_path) }, Core::File::OpenMode::Write); !file.is_error()) {
                if (auto write_result = file.value()->write_until_depleted(encoded.value()); write_result.is_error())
                    warnln("WindowServer: could not write the frame dump: {}", write_result.error());
            }
        }
    }

    auto* buffer = bitmap_buffer_create(move(bitmap));
    if (!toplevel.frame_buffer) {
        toplevel.frame_buffer = wlr_scene_buffer_create(toplevel.scene_tree, buffer);
        wlr_scene_node_lower_to_bottom(&toplevel.frame_buffer->node);
        wlr_scene_node_set_position(&toplevel.frame_buffer->node, 0, 0);
    } else {
        wlr_scene_buffer_set_buffer(toplevel.frame_buffer, buffer);
    }
    wlr_buffer_drop(buffer);
}

static void toplevel_map(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, map);
    toplevel_update_foreign(*toplevel);
    dbgln("WindowServer: toplevel mapped title='{}' app_id='{}'",
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "",
        toplevel->xdg_toplevel->app_id ? toplevel->xdg_toplevel->app_id : "");
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
    toplevel->frame_active = true;
    toplevel_update_frame(*toplevel);
    toplevel_report_position(*toplevel);
    if (toplevel->foreign_handle && toplevel->server->primary_output)
        wlr_foreign_toplevel_handle_v1_output_enter(toplevel->foreign_handle, toplevel->server->primary_output);
    focus_toplevel(*toplevel->server, toplevel);
    WindowManager::emit_window_state(*toplevel);
}

static void toplevel_unmap(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, unmap);
    // Hide the compositor-drawn frame along with the client's surface. The frame
    // buffer is a child of this tree and isn't tied to the wl_surface lifetime.
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    if (toplevel == toplevel->server->grabbed_toplevel)
        reset_cursor_mode(*toplevel->server);
}

// Spike (docs/09-r3-implementation-plan.md §3): capture a toplevel's pixels for
// the switcher's thumbnails. Set WINDOWSERVER_DUMP_THUMBNAIL=<dir> to dump one
// PNG per toplevel, so the capture path can be verified headlessly.
static void maybe_dump_thumbnail(Toplevel& toplevel)
{
    auto const* dir = getenv("WINDOWSERVER_DUMP_THUMBNAIL");
    if (!dir || !*dir)
        return;
    static HashTable<i32> dumped;
    if (dumped.contains(toplevel.wm_id))
        return;

    auto bitmap = snapshot_toplevel(toplevel.content_tree->node);
    if (!bitmap) {
        // Not ready yet (the initial commits have no buffer); retry on the next
        // commit rather than consuming the one-shot dump.
        return;
    }
    dumped.set(toplevel.wm_id);
    dbgln("WindowServer: thumbnail for wm_id={} is {}x{}", toplevel.wm_id, bitmap->width(), bitmap->height());
    if (auto encoded = Gfx::PNGWriter::encode(*bitmap); !encoded.is_error()) {
        auto path = ByteString::formatted("{}/thumb-{}.png", ByteString { dir }, toplevel.wm_id);
        if (auto file = Core::File::open(path, Core::File::OpenMode::Write); !file.is_error())
            (void)file.value()->write_until_depleted(encoded.value());
    }
}

static void toplevel_commit(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, commit);
    if (toplevel->xdg_toplevel->base->initial_commit)
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
    // A decoration created before the surface was initialized is configured now.
    if (toplevel->decoration && toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        toplevel->decoration = nullptr;
    }
    toplevel_update_foreign(*toplevel);
    toplevel_update_frame(*toplevel);
    maybe_dump_thumbnail(*toplevel);
    WindowManager::emit_window_state(*toplevel);
}

static void toplevel_destroy(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, destroy);
    auto& server = *toplevel->server;
    wl_list_remove(&toplevel->map.link);
    wl_list_remove(&toplevel->unmap.link);
    wl_list_remove(&toplevel->commit.link);
    wl_list_remove(&toplevel->destroy.link);
    wl_list_remove(&toplevel->request_move.link);
    wl_list_remove(&toplevel->request_resize.link);
    wl_list_remove(&toplevel->request_maximize.link);
    wl_list_remove(&toplevel->request_fullscreen.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->set_app_id.link);
    wl_list_remove(&toplevel->foreign_request_activate.link);
    wl_list_remove(&toplevel->foreign_request_close.link);
    if (toplevel->foreign_handle)
        wlr_foreign_toplevel_handle_v1_destroy(toplevel->foreign_handle);
    // The chrome resource may outlive the toplevel (the client owns it), so make
    // sure its destroy handler can't touch freed memory.
    if (toplevel->chrome)
        wl_resource_set_user_data(toplevel->chrome, nullptr);
    if (server.window_menu.open && server.window_menu.toplevel == toplevel)
        close_window_menu(server);
    // `scene_tree` is our own wrapper around the xdg surface's content tree (so
    // the frame buffer can share a node with the content). wlroots only tears
    // down the content tree it created, so without this the frame buffer keeps
    // rendering after the client is gone and the window appears to freeze on
    // screen. Destroying the wrapper also destroys the content child, which
    // detaches wlroots' listener before the xdg_surface itself goes away.
    wlr_scene_node_destroy(&toplevel->scene_tree->node);
    dbgln("WindowServer: toplevel destroyed wm_id={}", toplevel->wm_id);
    WindowManager::emit_window_removed(*toplevel);
    remove_owned(server.toplevels, toplevel);
}

static void toplevel_begin_interactive(Toplevel& toplevel, CursorMode mode, uint32_t edges)
{
    auto& server = *toplevel.server;
    server.grabbed_toplevel = &toplevel;
    server.cursor_mode = mode;
    if (mode == CursorMode::Move) {
        server.grab_x = server.cursor->x - toplevel.scene_tree->node.x;
        server.grab_y = server.cursor->y - toplevel.scene_tree->node.y;
    } else {
        auto* geometry = &toplevel.xdg_toplevel->base->geometry;
        double border_x = (toplevel.scene_tree->node.x + geometry->x) + ((edges & WLR_EDGE_RIGHT) ? geometry->width : 0);
        double border_y = (toplevel.scene_tree->node.y + geometry->y) + ((edges & WLR_EDGE_BOTTOM) ? geometry->height : 0);
        server.grab_x = server.cursor->x - border_x;
        server.grab_y = server.cursor->y - border_y;
        server.grab_box = *geometry;
        server.grab_box.x += toplevel.scene_tree->node.x;
        server.grab_box.y += toplevel.scene_tree->node.y;
        server.resize_edges = edges;
    }

    wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, mode == CursorMode::Move ? "grabbing" : resize_cursor_name(edges));
}

static void toplevel_close(Toplevel& toplevel)
{
    wlr_xdg_toplevel_send_close(toplevel.xdg_toplevel);
}

static void toplevel_toggle_maximized(Toplevel& toplevel)
{
    auto& server = *toplevel.server;
    // A rolled-up window can't be meaningfully maximized; unroll it first so the
    // frame metrics used below are those of the full window.
    if (toplevel.rolled_up) {
        toplevel.rolled_up = false;
        toplevel_update_frame(toplevel);
    }
    if (!toplevel.maximized) {
        toplevel.restore_size = { toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
        if (auto* output_state = output_for(server, server.primary_output); output_state && !wlr_box_empty(&output_state->usable_area))
            box = output_state->usable_area;
        toplevel.maximized = true;
        wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, true);
        if (server.primary_output)
            wlr_scene_node_set_position(&toplevel.scene_tree->node, box.x, box.y);
        toplevel_report_position(toplevel);
        wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, box.width - toplevel.frame_left - toplevel.frame_right, box.height - toplevel.frame_top - toplevel.frame_bottom);
    } else {
        toplevel.maximized = false;
        wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, false);
        wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, toplevel.restore_size.width(), toplevel.restore_size.height());
    }
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_maximized(toplevel.foreign_handle, toplevel.maximized);
}

static void toplevel_toggle_minimized(Toplevel& toplevel)
{
    toplevel.minimized = !toplevel.minimized;
    wlr_scene_node_set_enabled(&toplevel.scene_tree->node, !toplevel.minimized);
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel.foreign_handle, toplevel.minimized);
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

static void close_window_menu(Server& server)
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
    server.frame_grab = false;
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

static void update_window_menu_hover(Server& server, double x, double y)
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

static void window_menu_handle_press(Server& server, uint32_t button)
{
    auto& menu = server.window_menu;
    if (button == BTN_LEFT)
        menu.pressed_index = window_menu_index_at(menu, server.cursor->x, server.cursor->y);
    else
        close_window_menu(server);
}

static void window_menu_handle_release(Server& server, uint32_t button)
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

static bool window_menu_handle_key(Server& server, xkb_keysym_t sym)
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

static bool handle_frame_press(Server& server, uint32_t button, uint32_t time)
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
            server.double_click_pending = false;
            open_window_menu(server, *toplevel, Gfx::IntPoint { static_cast<int>(server.cursor->x), static_cast<int>(server.cursor->y) });
            return true;
        }
    }

    if (button == BTN_LEFT) {
        if (hit.region == FrameRegion::Button) {
            // Track the button until release, like SerenityOS's Button: the
            // action fires only if the cursor is still over it on mouse up.
            server.pressed_button_toplevel = toplevel;
            server.pressed_button_index = hit.button_index;
            server.double_click_pending = false;
            toplevel->hovered_button = hit.button_index;
            toplevel->pressed_button = hit.button_index;
            toplevel_update_frame(*toplevel);
            return true;
        }

        if (hit.region == FrameRegion::Titlebar) {
            focus_toplevel(server, toplevel);
            // Decide on release whether this is a double click; a first click
            // still starts a move so single-click dragging feels immediate.
            bool same_window = server.last_titlebar_press_toplevel == toplevel;
            double dx = server.cursor->x - server.last_titlebar_press_x;
            double dy = server.cursor->y - server.last_titlebar_press_y;
            server.double_click_pending = same_window
                && (time - server.last_titlebar_press_time) < 300
                && (dx * dx + dy * dy) <= (6 * 6);
            server.last_titlebar_press_toplevel = toplevel;
            server.last_titlebar_press_time = time;
            server.last_titlebar_press_x = server.cursor->x;
            server.last_titlebar_press_y = server.cursor->y;
            toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
            return true;
        }

        if (hit.region == FrameRegion::Border) {
            focus_toplevel(server, toplevel);
            server.double_click_pending = false;
            toplevel_begin_interactive(*toplevel, CursorMode::Resize, hit.edges);
            return true;
        }
    }

    return false;
}

static void handle_frame_release(Server& server, uint32_t)
{
    if (server.pressed_button_toplevel) {
        auto* toplevel = server.pressed_button_toplevel;
        int index = server.pressed_button_index;
        bool activate = toplevel->pressed_button == index;
        toplevel->pressed_button = -1;
        server.pressed_button_toplevel = nullptr;
        server.pressed_button_index = -1;
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

    if (server.cursor_mode == CursorMode::Move && server.double_click_pending) {
        if (auto* toplevel = server.grabbed_toplevel)
            toplevel_toggle_maximized(*toplevel);
    }
    server.double_click_pending = false;
    reset_cursor_mode(server);
}

static void toplevel_request_move(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_move);
    toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
}

static void toplevel_request_resize(wl_listener* listener, void* data)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_resize);
    auto* event = static_cast<wlr_xdg_toplevel_resize_event*>(data);
    toplevel_begin_interactive(*toplevel, CursorMode::Resize, event->edges);
}

static void toplevel_request_maximize(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_maximize);
    if (toplevel->xdg_toplevel->base->initialized)
        wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
}

static void toplevel_request_fullscreen(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_fullscreen);
    auto& server = *toplevel->server;
    if (!toplevel->xdg_toplevel->base->initialized)
        return;

    bool fullscreen = toplevel->xdg_toplevel->requested.fullscreen;
    dbgln("WindowServer: request_fullscreen={} title='{}'", fullscreen,
        toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "");
    // Acknowledge the request. The client only learns it is fullscreen from the
    // configure we send back, so both the state and the size have to be set.
    wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, fullscreen);

    wlr_box box { };
    if (server.primary_output)
        wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);

    if (fullscreen) {
        toplevel->restore_size = { toplevel->xdg_toplevel->base->geometry.width, toplevel->xdg_toplevel->base->geometry.height };
        toplevel->restore_position = { toplevel->scene_tree->node.x, toplevel->scene_tree->node.y };
        toplevel->fullscreen = true;
        if (!wlr_box_empty(&box)) {
            wlr_scene_node_set_position(&toplevel->scene_tree->node, box.x, box.y);
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, box.width, box.height);
        }
    } else {
        toplevel->fullscreen = false;
        wlr_scene_node_set_position(&toplevel->scene_tree->node, toplevel->restore_position.x(), toplevel->restore_position.y());
        wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->restore_size.width(), toplevel->restore_size.height());
    }
    toplevel_update_frame(*toplevel);
}

static void toplevel_set_title(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_title);
    toplevel_update_foreign(*toplevel);
}

static void toplevel_set_app_id(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_app_id);
    toplevel_update_foreign(*toplevel);
}

static void toplevel_foreign_request_activate(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, foreign_request_activate);
    focus_toplevel(*toplevel->server, toplevel);
    WindowManager::emit_window_state(*toplevel);
}

static void toplevel_foreign_request_close(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, foreign_request_close);
    wlr_xdg_toplevel_send_close(toplevel->xdg_toplevel);
}

static void server_new_xdg_toplevel(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_xdg_toplevel);
    auto* xdg_toplevel = static_cast<wlr_xdg_toplevel*>(data);

    auto toplevel = make<Toplevel>();
    toplevel->server = server;
    toplevel->xdg_toplevel = xdg_toplevel;
    toplevel->scene_tree = wlr_scene_tree_create(server->windows_tree);
    toplevel->scene_tree->node.data = toplevel.ptr();
    toplevel->content_tree = wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
    xdg_toplevel->base->data = toplevel->content_tree;

    if (server->foreign_toplevel_manager) {
        toplevel->foreign_handle = wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
        toplevel->foreign_handle->data = toplevel.ptr();
        toplevel->foreign_request_activate.notify = toplevel_foreign_request_activate;
        wl_signal_add(&toplevel->foreign_handle->events.request_activate, &toplevel->foreign_request_activate);
        toplevel->foreign_request_close.notify = toplevel_foreign_request_close;
        wl_signal_add(&toplevel->foreign_handle->events.request_close, &toplevel->foreign_request_close);
    }

    toplevel->map.notify = toplevel_map;
    wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
    toplevel->unmap.notify = toplevel_unmap;
    wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
    toplevel->commit.notify = toplevel_commit;
    wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
    toplevel->destroy.notify = toplevel_destroy;
    wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
    toplevel->request_move.notify = toplevel_request_move;
    wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
    toplevel->request_resize.notify = toplevel_request_resize;
    wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
    toplevel->request_maximize.notify = toplevel_request_maximize;
    wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
    toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
    wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
    toplevel->set_title.notify = toplevel_set_title;
    wl_signal_add(&xdg_toplevel->events.set_title, &toplevel->set_title);
    toplevel->set_app_id.notify = toplevel_set_app_id;
    wl_signal_add(&xdg_toplevel->events.set_app_id, &toplevel->set_app_id);

    server->toplevels.append(move(toplevel));
    server->toplevels.last()->wm_id = g_next_wm_id++;
}

static void popup_commit(wl_listener* listener, void*)
{
    Popup* popup = nullptr;
    popup = wl_container_of(listener, popup, commit);
    if (popup->xdg_popup->base->initial_commit)
        wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
}

static void popup_destroy(wl_listener* listener, void*)
{
    Popup* popup = nullptr;
    popup = wl_container_of(listener, popup, destroy);
    wl_list_remove(&popup->commit.link);
    wl_list_remove(&popup->destroy.link);
    remove_owned(popup->server->popups, popup);
}

static void server_new_xdg_popup(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_xdg_popup);
    auto* xdg_popup = static_cast<wlr_xdg_popup*>(data);

    auto popup = make<Popup>();
    popup->server = server;
    popup->xdg_popup = xdg_popup;

    auto* parent = wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
    if (!parent)
        return;
    auto* parent_tree = static_cast<wlr_scene_tree*>(parent->data);
    xdg_popup->base->data = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

    popup->commit.notify = popup_commit;
    wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
    popup->destroy.notify = popup_destroy;
    wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);

    server->popups.append(move(popup));
}

// --- layer shell ----------------------------------------------------------

static Output* output_for(Server& server, wlr_output* output)
{
    for (auto& candidate : server.outputs) {
        if (candidate->output == output)
            return candidate.ptr();
    }
    return nullptr;
}

static void arrange_layers(Server& server, wlr_output* output)
{
    if (!output)
        return;

    wlr_box full_area { };
    wlr_output_layout_get_box(server.output_layout, output, &full_area);
    wlr_box usable_area = full_area;

    auto* output_state = output_for(server, output);

    // Configure layer surfaces from the bottom-most layer up, so exclusive
    // zones from lower layers shrink the area available to higher ones.
    static constexpr zwlr_layer_shell_v1_layer layer_order[] = {
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND,
        ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM,
        ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
    };

    for (auto layer : layer_order) {
        for (auto& layer_surface : server.layer_surfaces) {
            auto* surface = layer_surface->layer_surface;
            if (surface->output != output)
                continue;
            if (surface->current.layer != layer)
                continue;
            wlr_scene_layer_surface_v1_configure(layer_surface->scene, &full_area, &usable_area);
        }
    }

    if (output_state
        && (output_state->usable_area.width != usable_area.width
            || output_state->usable_area.height != usable_area.height
            || output_state->usable_area.x != usable_area.x
            || output_state->usable_area.y != usable_area.y)) {
        output_state->usable_area = usable_area;
        dbgln("WindowServer: usable area for '{}': {}x{} at {},{}", output->name ? output->name : "?",
            usable_area.width, usable_area.height, usable_area.x, usable_area.y);
    }
}

static void layer_surface_commit(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, commit);
    arrange_layers(*layer->server, layer->output);
}

static void layer_surface_map(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, map);
    wlr_scene_node_set_enabled(&layer->scene->tree->node, true);
    arrange_layers(*layer->server, layer->output);
}

static void layer_surface_unmap(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, unmap);
    wlr_scene_node_set_enabled(&layer->scene->tree->node, false);
    arrange_layers(*layer->server, layer->output);
}

static void layer_surface_destroy(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, destroy);
    wl_list_remove(&layer->map.link);
    wl_list_remove(&layer->unmap.link);
    wl_list_remove(&layer->commit.link);
    wl_list_remove(&layer->destroy.link);
    auto* server = layer->server;
    auto* output = layer->output;
    remove_owned(server->layer_surfaces, layer);
    arrange_layers(*server, output);
}

static void server_new_layer_surface(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_layer_surface);
    auto* layer_surface = static_cast<wlr_layer_surface_v1*>(data);

    if (!layer_surface->output)
        layer_surface->output = server->primary_output;
    if (!layer_surface->output) {
        wlr_layer_surface_v1_destroy(layer_surface);
        return;
    }

    auto layer = make<LayerSurface>();
    layer->server = server;
    layer->layer_surface = layer_surface;
    layer->output = layer_surface->output;
    layer->is_menu = layer_surface->layer_namespace && !strcmp(layer_surface->layer_namespace, "serenity-menu");
    dbgln("WindowServer: layer surface namespace='{}'", layer_surface->layer_namespace ? layer_surface->layer_namespace : "");
    wlr_scene_tree* layer_parent = server->background_tree;
    switch (layer_surface->pending.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        layer_parent = server->background_tree;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        layer_parent = server->bottom_tree;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        layer_parent = server->top_tree;
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        layer_parent = server->overlay_tree;
        break;
    default:
        break;
    }
    layer->scene = wlr_scene_layer_surface_v1_create(layer_parent, layer_surface);
    layer->scene->tree->node.data = layer.ptr();
    layer_surface->data = layer.ptr();

    layer->map.notify = layer_surface_map;
    wl_signal_add(&layer_surface->surface->events.map, &layer->map);
    layer->unmap.notify = layer_surface_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);
    layer->commit.notify = layer_surface_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &layer->commit);
    layer->destroy.notify = layer_surface_destroy;
    wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

    server->layer_surfaces.append(move(layer));
}

// --- xdg-decoration -------------------------------------------------------

static void decoration_request_mode(wl_listener* listener, void*)
{
    Decoration* decoration = nullptr;
    decoration = wl_container_of(listener, decoration, request_mode);
    // Configure only once the surface is initialized; the initial commit applies
    // the default mode otherwise.
    if (!decoration->decoration->toplevel->base->initialized)
        return;
    auto requested = decoration->decoration->requested_mode;
    // Strongly prefer server-side decorations, but honour an explicit CSD request.
    if (requested == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE)
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration->decoration, requested);
    else
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void decoration_destroy(wl_listener* listener, void*)
{
    Decoration* decoration = nullptr;
    decoration = wl_container_of(listener, decoration, destroy);
    wl_list_remove(&decoration->request_mode.link);
    wl_list_remove(&decoration->destroy.link);
    if (decoration->toplevel && decoration->toplevel->decoration == decoration->decoration)
        decoration->toplevel->decoration = nullptr;
    remove_owned(decoration->server->decorations, decoration);
}

static void server_new_toplevel_decoration(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_toplevel_decoration);
    auto* wlr_decoration = static_cast<wlr_xdg_toplevel_decoration_v1*>(data);

    auto decoration = make<Decoration>();
    decoration->server = server;
    decoration->decoration = wlr_decoration;
    decoration->request_mode.notify = decoration_request_mode;
    wl_signal_add(&wlr_decoration->events.request_mode, &decoration->request_mode);
    decoration->destroy.notify = decoration_destroy;
    wl_signal_add(&wlr_decoration->events.destroy, &decoration->destroy);

    auto* xdg_toplevel = wlr_decoration->toplevel;
    for (auto& candidate : server->toplevels) {
        if (candidate->xdg_toplevel == xdg_toplevel) {
            decoration->toplevel = candidate.ptr();
            break;
        }
    }

    // Prefer server-side decorations. `set_mode` schedules a configure and
    // asserts the surface is initialized, so defer to the first commit if it is
    // created before the toplevel is initialized.
    if (xdg_toplevel->base->initialized)
        wlr_xdg_toplevel_decoration_v1_set_mode(wlr_decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    else if (decoration->toplevel)
        decoration->toplevel->decoration = wlr_decoration;
    server->decorations.append(move(decoration));
}

// --- output management ----------------------------------------------------

static void output_manager_send_configuration(Server& server)
{
    if (!server.output_manager)
        return;

    auto* config = wlr_output_configuration_v1_create();
    for (auto& output : server.outputs) {
        auto* head = wlr_output_configuration_head_v1_create(config, output->output);
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, output->output, &box);
        head->state.x = box.x;
        head->state.y = box.y;
    }
    wlr_output_manager_v1_set_configuration(server.output_manager, config);
}

static void output_manager_apply_or_test(Server& server, wlr_output_configuration_v1* config, bool test)
{
    size_t states_len = 0;
    auto* states = wlr_output_configuration_v1_build_state(config, &states_len);
    bool ok = states != nullptr
        && (test ? wlr_backend_test(server.backend, states, states_len)
                 : wlr_backend_commit(server.backend, states, states_len));
    free(states);

    if (!ok) {
        wlr_output_configuration_v1_send_failed(config);
        wlr_output_configuration_v1_destroy(config);
        return;
    }

    if (!test) {
        wlr_output_configuration_head_v1* head = nullptr;
        wl_list_for_each(head, &config->heads, link)
        {
            if (head->state.enabled)
                wlr_output_layout_add(server.output_layout, head->state.output, head->state.x, head->state.y);
        }
    }

    wlr_output_configuration_v1_send_succeeded(config);
    wlr_output_configuration_v1_destroy(config);

    if (!test)
        output_manager_send_configuration(server);
}

static void output_manager_apply(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, output_manager_apply);
    output_manager_apply_or_test(*server, static_cast<wlr_output_configuration_v1*>(data), false);
}

static void output_manager_test(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, output_manager_test);
    output_manager_apply_or_test(*server, static_cast<wlr_output_configuration_v1*>(data), true);
}


// --- serenity-window (compositor -> LibWindowServer chrome control) --------

static void serenity_toplevel_resource_destroy(wl_resource* resource)
{
    auto* toplevel = static_cast<Toplevel*>(wl_resource_get_user_data(resource));
    if (toplevel && toplevel->chrome == resource)
        toplevel->chrome = nullptr;
}

static void serenity_toplevel_request_destroy(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static struct serenity_toplevel_interface const s_serenity_toplevel_implementation = {
    .destroy = serenity_toplevel_request_destroy,
};

static void serenity_window_manager_get_toplevel(wl_client* client, wl_resource* resource, uint32_t id, wl_resource* surface_resource)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    auto* wlr_surface = wlr_surface_from_resource(surface_resource);

    Toplevel* toplevel = nullptr;
    for (auto& candidate : server.toplevels) {
        if (candidate->xdg_toplevel && candidate->xdg_toplevel->base->surface == wlr_surface) {
            toplevel = candidate.ptr();
            break;
        }
    }
    if (!toplevel) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "unknown wl_surface");
        return;
    }

    auto* chrome = wl_resource_create(client, &serenity_toplevel_interface, 1, id);
    if (!chrome) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(chrome, &s_serenity_toplevel_implementation, toplevel, serenity_toplevel_resource_destroy);
    toplevel->chrome = chrome;

    // Bring the client up to date with the current menubar visibility.
    if (!toplevel->menubar_visible)
        serenity_toplevel_send_menubar_visibility(chrome, 0);
}

static void serenity_window_manager_resource_destroy(wl_resource* resource)
{
    if (auto* server = static_cast<Server*>(wl_resource_get_user_data(resource)))
        server->serenity_window_managers.remove_first_matching([&](auto* candidate) { return candidate == resource; });
}

static void serenity_window_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto& server = *static_cast<Server*>(data);
    auto* resource = wl_resource_create(client, &serenity_window_manager_interface, min(version, 1u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    static struct serenity_window_manager_interface const implementation = {
        .get_toplevel = serenity_window_manager_get_toplevel,
    };
    wl_resource_set_implementation(resource, &implementation, &server, serenity_window_manager_resource_destroy);
    server.serenity_window_managers.append(resource);
}

// --- setup ----------------------------------------------------------------

static ErrorOr<void> initialize(Server& server)
{
    server.display = wl_display_create();
    if (!server.display)
        return Error::from_string_literal("failed to create wl_display");

    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), nullptr);
    if (!server.backend)
        return Error::from_string_literal("failed to create wlr_backend");

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer)
        return Error::from_string_literal("failed to create wlr_renderer");
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator)
        return Error::from_string_literal("failed to create wlr_allocator");

    wlr_compositor_create(server.display, 5, server.renderer);
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.output_layout = wlr_output_layout_create(server.display);
    server.scene = wlr_scene_create();
    server.scene_layout = wlr_scene_attach_output_layout(server.scene, server.output_layout);

    // Layer trees, created bottom-to-top: background, bottom, windows, top,
    // overlay. Layer surfaces go into the tree for their layer and ordinary
    // windows into windows_tree, so stacking is correct.
    server.background_tree = wlr_scene_tree_create(&server.scene->tree);
    server.bottom_tree = wlr_scene_tree_create(&server.scene->tree);
    server.windows_tree = wlr_scene_tree_create(&server.scene->tree);
    server.top_tree = wlr_scene_tree_create(&server.scene->tree);
    server.overlay_tree = wlr_scene_tree_create(&server.scene->tree);
    wlr_xdg_output_manager_v1_create(server.display, server.output_layout);

    server.new_output.notify = server_new_output;
    wl_signal_add(&server.backend->events.new_output, &server.new_output);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
    wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
    server.new_xdg_popup.notify = server_new_xdg_popup;
    wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

    server.layer_shell = wlr_layer_shell_v1_create(server.display, 4);
    server.new_layer_surface.notify = server_new_layer_surface;
    wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

    if (auto* decoration_manager = wlr_xdg_decoration_manager_v1_create(server.display)) {
        server.new_toplevel_decoration.notify = server_new_toplevel_decoration;
        wl_signal_add(&decoration_manager->events.new_toplevel_decoration, &server.new_toplevel_decoration);
    }

    server.foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server.display);
    wl_global_create(server.display, &serenity_window_manager_interface, 1, &server, serenity_window_manager_bind);

    server.output_manager = wlr_output_manager_v1_create(server.display);
    server.output_manager_apply.notify = output_manager_apply;
    wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
    server.output_manager_test.notify = output_manager_test;
    wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);

    // Virtual input for the headless harness (docs/09-r3-implementation-plan.md §2.4).
    server.virtual_keyboard_manager = wlr_virtual_keyboard_manager_v1_create(server.display);
    server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
    wl_signal_add(&server.virtual_keyboard_manager->events.new_virtual_keyboard, &server.new_virtual_keyboard);
    server.virtual_pointer_manager = wlr_virtual_pointer_manager_v1_create(server.display);
    server.new_virtual_pointer.notify = server_new_virtual_pointer;
    wl_signal_add(&server.virtual_pointer_manager->events.new_virtual_pointer, &server.new_virtual_pointer);

    server.cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
    server.cursor_manager = wlr_xcursor_manager_create(nullptr, 24);
    wlr_xcursor_manager_load(server.cursor_manager, 1);
    wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");

    server.cursor_motion.notify = cursor_motion;
    wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
    server.cursor_motion_absolute.notify = cursor_motion_absolute;
    wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
    server.cursor_button.notify = cursor_button;
    wl_signal_add(&server.cursor->events.button, &server.cursor_button);
    server.cursor_axis.notify = cursor_axis;
    wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
    server.cursor_frame.notify = cursor_frame;
    wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.seat = wlr_seat_create(server.display, "seat0");
    server.seat_request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.seat_request_cursor);
    server.seat_pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.seat_pointer_focus_change);
    server.seat_request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.seat_request_set_selection);

    return { };
}

static ByteString add_socket(Server& server, ByteString const& requested)
{
    if (!requested.is_empty()) {
        if (wl_display_add_socket(server.display, requested.characters()) == 0)
            return requested;
        warnln("WindowServer: could not add socket '{}'", requested);
    }
    auto const* socket = wl_display_add_socket_auto(server.display);
    return socket ? ByteString(socket) : ByteString { };
}

static void spawn_startup_command(StringView command)
{
    if (command.is_empty())
        return;
    auto command_string = command.to_byte_string();
    if (fork() == 0) {
        execl("/bin/sh", "/bin/sh", "-c", command_string.characters(), static_cast<char*>(nullptr));
        _exit(127);
    }
}

static ErrorOr<void> initialize_gfx()
{
    auto const* res_root = getenv("SERENITY_RES_ROOT");
    if (!res_root || !*res_root)
        return Error::from_string_literal("SERENITY_RES_ROOT is not set; cannot load the Serenity theme");

    ByteString resource_root { res_root };
    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(TRY(String::from_byte_string(resource_root))));

    Gfx::FontDatabase::set_default_font_query("Katica 10 400 0"sv);
    Gfx::FontDatabase::set_window_title_font_query("Katica 10 700 0"sv);
    Gfx::FontDatabase::the().load_all_fonts_from_uri("resource://fonts"sv);

    auto theme = TRY(Gfx::load_system_theme(ByteString::formatted("{}/themes/Default.ini", resource_root)));
    Gfx::set_system_theme(theme);
    g_palette = new Gfx::Palette(Gfx::PaletteImpl::create_with_anonymous_buffer(theme));

    g_frame_icons.close = load_icon(resource_root, "window-close"sv, "window-close"sv);
    g_frame_icons.minimize = load_icon(resource_root, "window-minimize"sv, "downward-triangle"sv);
    g_frame_icons.maximize = load_icon(resource_root, "window-maximize"sv, "upward-triangle"sv);
    g_frame_icons.restore = load_icon(resource_root, "window-restore"sv, "upward-triangle"sv);
    g_frame_icons.move = load_icon(resource_root, "move"sv, ""sv);
    g_frame_icons.pin = load_icon(resource_root, "window-pin"sv, ""sv);

    // Wallpaper/background: load the persisted state, seeding from
    // SDE_WALLPAPER / SDE_WALLPAPER_MODE on the first run.
    initialize_wallpaper(resource_root);

    // The compositor's own configuration (WindowServer.ini) lives beside the
    // wallpaper/theme state; load it now and reload on change.
    Config::load();
    Config::watch([] {});

    dbgln("WindowServer: loaded the Serenity theme from {}", resource_root);
    return { };
}

static ByteString theme_broker_path()
{
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-theme", runtime);
    return ByteString { "/tmp/sde-theme" };
}

static void apply_theme_path(Server& server, ByteString const& theme_path)
{
    auto theme = Gfx::load_system_theme(theme_path, {});
    if (theme.is_error()) {
        warnln("WindowServer: could not load theme '{}': {}", theme_path, theme.error());
        return;
    }
    Gfx::set_system_theme(theme.value());
    delete g_palette;
    g_palette = new Gfx::Palette(Gfx::PaletteImpl::create_with_anonymous_buffer(theme.value()));
    for (auto& toplevel : server.toplevels)
        toplevel_update_frame(*toplevel);
    for (auto& output : server.outputs) {
        output->background_size = { };
        update_desktop_background(server, *output);
    }
    dbgln("WindowServer: applied system theme '{}'", theme_path);
}

static void watch_theme_broker(Server& server)
{
    static RefPtr<Core::FileWatcher> watcher;
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto path = theme_broker_path();
    watcher->on_change = [&server, path](Core::FileWatcherEvent const& event) {
        if (event.event_path != path)
            return;
        auto file = Core::File::open(path, Core::File::OpenMode::Read);
        if (file.is_error())
            return;
        auto contents = file.value()->read_until_eof();
        if (contents.is_error())
            return;
        auto theme_path = StringView { contents.value() }.trim_whitespace().to_byte_string();
        if (!theme_path.is_empty())
            apply_theme_path(server, theme_path);
    };
    if (auto directory = LexicalPath(path).dirname(); !directory.is_empty())
        (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    // Ensure the file exists so we can watch its content; a write is IN_MODIFY
    // (ContentModified), which a directory watch does not report.
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}

}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    wlr_log_init(WLR_ERROR, nullptr);

    StringView startup_command;
    StringView socket_name;
    Core::ArgsParser args_parser;
    args_parser.add_option(startup_command, "Command to run once the compositor is up", nullptr, 's', "command");
    args_parser.add_option(socket_name, "Wayland socket name", nullptr, 'n', "name");
    args_parser.parse(arguments);

    TRY(initialize_gfx());

    auto server = make<Server>();
    if (auto result = initialize(*server); result.is_error()) {
        warnln("WindowServer: {}", result.error());
        return 1;
    }

    auto socket = add_socket(*server, socket_name.to_byte_string());
    if (socket.is_empty()) {
        warnln("WindowServer: failed to create a Wayland socket");
        return 1;
    }

    if (!wlr_backend_start(server->backend)) {
        warnln("WindowServer: failed to start the backend");
        wl_display_destroy(server->display);
        return 1;
    }

    setenv("WAYLAND_DISPLAY", socket.characters(), true);
    dbgln("WindowServer: running on WAYLAND_DISPLAY={}", socket);
    spawn_startup_command(startup_command);

    // The WindowServer serves the WindowManagerServer protocol (the shell's
    // Taskbar protocol), just like SerenityOS's WindowServer does. The socket
    // path can be overridden so tests can run isolated from a live session.
    ByteString wm_portal { "/tmp/portal/wm" };
    if (auto const* override_path = getenv("SDE_WM_PORTAL_PATH"); override_path && *override_path) {
        wm_portal = override_path;
    } else {
        ::mkdir("/tmp/portal", 0755);
    }
    ::unlink(wm_portal.characters());
    auto wm_server = Core::LocalServer::construct();
    if (!wm_server->listen(wm_portal))
        warnln("WindowServer: could not listen on {}", wm_portal);
    // Let the window manager call back into the compositor's focus/menu code.
    WindowManager::set_host_hooks({ focus_toplevel, open_window_menu });
    wm_server->on_accept = [&](NonnullOwnPtr<Core::LocalSocket> socket) {
        WindowManager::accept_connection(*server, move(socket));
    };

    // One event loop for both Wayland events (via a notifier) and the in-process
    // WindowManagerServer IPC connections.
    Core::EventLoop loop;
    auto* wayland_loop = wl_display_get_event_loop(server->display);
    auto display_notifier = Core::Notifier::construct(wl_event_loop_get_fd(wayland_loop), Core::Notifier::Type::Read);
    display_notifier->on_activation = [&] {
        wl_event_loop_dispatch(wayland_loop, 0);
        wl_display_flush_clients(server->display);
    };

    watch_theme_broker(*server);
    watch_wallpaper_state(*server);

    int result = loop.exec();
    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);
    return result;
}
