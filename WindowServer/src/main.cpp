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
#include <AK/StringBuilder.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StdLibExtras.h>
#include <AK/Vector.h>

#include "Config.h"
#include "Model.h"
#include "Overlays.h"
#include "AppletManager.h"
#include "SessionManager.h"
#include "WindowManager.h"
#include "WindowSwitcher.h"

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
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <wlr/types/wlr_ext_foreign_toplevel_list_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_ext_image_capture_source_v1.h>
#include <wlr/types/wlr_ext_image_copy_capture_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/backend/libinput.h>
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

// Idle notification (R4.1): reset the seat's idle timer on input.
static void notify_activity(Server& server);
// While the session is locked, input goes to the lock surface on the output
// under the cursor.
static LockSurface* lock_surface_at_cursor(Server& server);
// Re-configure the lock surface for an output whose size/scale changed (R4.1).
static void reconfigure_lock_surface(Server& server, wlr_output* output);
// Spawn the configured lock client (R4.1); a standard ext-session-lock-v1 client.
static void spawn_lock_client();

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
static void persist_screen_layout(Server&);
static i32 g_next_wm_id { 1 };
static u64 g_focus_order_counter { 0 };
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

    toplevel->focus_order = ++g_focus_order_counter;

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

// Map wlroots modifier bits to Config's neutral modifier mask.
static u32 config_modifiers(uint32_t wlr_modifiers)
{
    u32 modifiers = 0;
    if (wlr_modifiers & WLR_MODIFIER_CTRL)
        modifiers |= Config::ModCtrl;
    if (wlr_modifiers & WLR_MODIFIER_ALT)
        modifiers |= Config::ModAlt;
    if (wlr_modifiers & WLR_MODIFIER_SHIFT)
        modifiers |= Config::ModShift;
    if (wlr_modifiers & WLR_MODIFIER_LOGO)
        modifiers |= Config::ModSuper;
    return modifiers;
}

// Run a configured keybinding action (R4.4).
static bool perform_key_action(Server& server, Config::Action action)
{
    if (action == Config::Action::Lock) {
        spawn_lock_client();
        return true;
    }
    return WindowManager::perform_action(server, action);
}

// Report a GlobalShortcuts activation to the portal backend over the session FIFO
// it watches (R5).
static void report_global_shortcut(StringView session, StringView id)
{
    ByteString path;
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        path = ByteString::formatted("{}/sde-global-shortcuts.fifo", runtime);
    else
        path = ByteString { "/tmp/sde-global-shortcuts.fifo" };
    int fd = ::open(path.characters(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;
    auto message = ByteString::formatted("{}\t{}\n", session, id);
    (void)::write(fd, message.characters(), message.length());
    ::close(fd);
    dbgln("WindowServer: global shortcut activated ('{}' {})", id, session);
}

static bool handle_keybinding(Server& server, xkb_keysym_t sym, uint32_t modifiers)
{
    // Alt+Escape quits the compositor (development convenience).
    if (modifiers & WLR_MODIFIER_ALT) {
        switch (sym) {
        case XKB_KEY_Escape:
            wl_display_terminate(server.display);
            return true;
        default:
            break;
        }
    }
    return false;
}

static void keyboard_key(wl_listener* listener, void* data)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, key);
    auto& server = *keyboard->server;
    auto* event = static_cast<wlr_keyboard_key_event*>(data);

    notify_activity(server);
    uint32_t keycode = event->keycode + 8;

    // Update the keyboard's xkb state ourselves (we handle keys before the seat
    // does), so the modifier mask is correct for keybindings and for the client's
    // wl_keyboard.modifiers.
    enum xkb_key_direction direction = event->state == WL_KEYBOARD_KEY_STATE_PRESSED ? XKB_KEY_DOWN : XKB_KEY_UP;
    auto* xkb_state = keyboard->keyboard->xkb_state;
    xkb_state_update_key(xkb_state, keycode, direction);
    keyboard->keyboard->modifiers.depressed = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_DEPRESSED);
    keyboard->keyboard->modifiers.latched = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LATCHED);
    keyboard->keyboard->modifiers.locked = xkb_state_serialize_mods(xkb_state, XKB_STATE_MODS_LOCKED);
    keyboard->keyboard->modifiers.group = xkb_state_serialize_layout(xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
    wlr_seat_keyboard_notify_modifiers(server.seat, &keyboard->keyboard->modifiers);

    // While the session is locked, keys go to the lock surface, not the shell.
    if (server.locked) {
        if (auto* lock = lock_surface_at_cursor(server)) {
            wlr_seat_set_keyboard(server.seat, keyboard->keyboard);
            wlr_seat_keyboard_notify_enter(server.seat, lock->lock_surface->surface, keyboard->keyboard->keycodes, keyboard->keyboard->num_keycodes, &keyboard->keyboard->modifiers);
            wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
        }
        return;
    }

    xkb_keysym_t const* syms = nullptr;
    int syms_count = xkb_state_key_get_syms(xkb_state, keycode, &syms);

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
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // R4.4: configured keybindings (defaults match the historical bindings).
        u32 const config_mods = config_modifiers(modifiers);
        for (int i = 0; i < syms_count; ++i) {
            if (auto action = Config::action_for(syms[i], config_mods); action.has_value())
                handled |= perform_key_action(server, action.value());
        }
        // R5: app global shortcuts (GlobalShortcuts portal).
        for (int i = 0; i < syms_count; ++i) {
            for (auto const& shortcut : Config::global_shortcuts()) {
                if (shortcut.keysym == syms[i] && shortcut.modifiers == config_mods) {
                    report_global_shortcut(shortcut.session, shortcut.id);
                    handled = true;
                    break;
                }
            }
        }
    }
    {
        // The window switcher (Alt+Tab / Super+Tab) grabs the keyboard while open.
        bool switcher_handled = false;
        for (int i = 0; i < syms_count; ++i)
            switcher_handled |= WindowSwitcher::handle_key(server, syms[i], modifiers, event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
        if (switcher_handled)
            return;
    }
    for (int i = 0; i < syms_count; ++i)
        handled |= WindowManager::handle_super_key(server, syms[i], modifiers, event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        for (int i = 0; i < syms_count; ++i)
            handled |= handle_keybinding(server, syms[i], modifiers);
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
    // The interactive grab ended: hide the geometry HUD (WS2).
    WindowManager::hide_geometry_hud();
}

static void process_cursor_move(Server& server)
{
    auto* toplevel = server.grabbed_toplevel;
    if (!toplevel)
        return;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, server.cursor->x - server.grab_x, server.cursor->y - server.grab_y);
    // Show the tile preview when the pointer reaches a screen edge (WS2).
    WindowManager::update_move_snap(server, *toplevel);
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
    // Show the geometry HUD with the intended size while resizing (WS2).
    WindowManager::update_resize_hud(server, *toplevel, { new_left, new_top, new_right - new_left, new_bottom - new_top });
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

// --- R4.7: cursor highlight -------------------------------------------------
//
// A translucent ellipse behind the cursor, from cursor.ini [Highlight] (a radius
// of at least 20 enables it, matching Mouse Settings). Drawn as an overlay scene
// buffer so it composites over windows but under the cursor itself.
static int g_cursor_highlight_radius { 0 };
static Gfx::Color g_cursor_highlight_color { Gfx::Color::Transparent };
static OwnPtr<ChromeNode> s_cursor_highlight;

static void render_cursor_highlight(Server& server)
{
    bool const enabled = g_cursor_highlight_radius >= 20 && g_cursor_highlight_color.alpha() != 0;
    if (!enabled) {
        if (s_cursor_highlight)
            s_cursor_highlight->set_visible(false);
        return;
    }
    if (!server.overlay_tree || !server.cursor)
        return;
    if (!s_cursor_highlight)
        s_cursor_highlight = make<ChromeNode>(*server.overlay_tree);
    int const radius = g_cursor_highlight_radius;
    s_cursor_highlight->set_rect({ static_cast<int>(server.cursor->x) - radius, static_cast<int>(server.cursor->y) - radius, radius * 2, radius * 2 });
    auto color = g_cursor_highlight_color;
    s_cursor_highlight->render([radius, color](Gfx::Painter& painter) {
        painter.fill_ellipse({ 0, 0, radius * 2, radius * 2 }, color);
    });
    s_cursor_highlight->set_visible(true);
    s_cursor_highlight->raise_to_top();
}

static void move_cursor_highlight(Server& server)
{
    if (!s_cursor_highlight || !s_cursor_highlight->is_visible() || !server.cursor)
        return;
    int const radius = g_cursor_highlight_radius;
    s_cursor_highlight->set_rect({ static_cast<int>(server.cursor->x) - radius, static_cast<int>(server.cursor->y) - radius, radius * 2, radius * 2 });
}

static void process_cursor_motion(Server& server, uint32_t time)
{
    // While the session is locked, only the lock surface receives input.
    if (server.locked) {
        if (auto* lock = lock_surface_at_cursor(server)) {
            wlr_box box { };
            wlr_output_layout_get_box(server.output_layout, lock->lock_surface->output, &box);
            double x = server.cursor->x - box.x;
            double y = server.cursor->y - box.y;
            wlr_seat_pointer_notify_enter(server.seat, lock->lock_surface->surface, x, y);
            wlr_seat_pointer_notify_motion(server.seat, time, x, y);
        }
        return;
    }

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

    // [Focus] FollowsMouse (R4.4): focus the toplevel under the cursor.
    if (Config::focus().follows_mouse && server.cursor_mode == CursorMode::Passthrough && !server.pointer_grab_surface) {
        double focus_x = 0;
        double focus_y = 0;
        wlr_surface* focus_surface = nullptr;
        if (auto* toplevel = toplevel_at(server, server.cursor->x, server.cursor->y, &focus_surface, &focus_x, &focus_y))
            focus_toplevel(server, toplevel);
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
    move_cursor_highlight(*server);
    process_cursor_motion(*server, event->time_msec);
    notify_activity(*server);
}

static void cursor_motion_absolute(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_motion_absolute);
    auto* event = static_cast<wlr_pointer_motion_absolute_event*>(data);
    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);
    move_cursor_highlight(*server);
    process_cursor_motion(*server, event->time_msec);
    notify_activity(*server);
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

    // R4.6: left-handed mode swaps the primary and secondary buttons.
    if (Config::input().mouse.buttons_switched) {
        if (event->button == BTN_LEFT)
            event->button = BTN_RIGHT;
        else if (event->button == BTN_RIGHT)
            event->button = BTN_LEFT;
    }

    notify_activity(*server);

    if (server->locked) {
        if (auto* lock = lock_surface_at_cursor(*server)) {
            wlr_box box { };
            wlr_output_layout_get_box(server->output_layout, lock->lock_surface->output, &box);
            double x = server->cursor->x - box.x;
            double y = server->cursor->y - box.y;
            wlr_seat_pointer_notify_enter(server->seat, lock->lock_surface->surface, x, y);
        }
        wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
        return;
    }

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
        // A titlebar drag has ended: tile to the previewed edge target, if any.
        if (server->grabbed_toplevel && server->cursor_mode == CursorMode::Move)
            WindowManager::finish_move_snap(*server, *server->grabbed_toplevel);
        handle_frame_release(*server, event->button);
        return;
    }

    // Dismiss client menus (opened from layer surfaces) on an outside press.
    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED)
        maybe_dismiss_layer_menus(*server);

    auto serial = wlr_seat_pointer_notify_button(server->seat, event->time_msec, event->button, event->state);
    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
        // A drag has ended: tile to the previewed edge target, if any (WS2).
        if (server->grabbed_toplevel && server->cursor_mode == CursorMode::Move)
            WindowManager::finish_move_snap(*server, *server->grabbed_toplevel);
        // Tell the client where its window ended up.
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
    // R4.6: natural-scroll inverts the axis; the scroll step scales it.
    auto const& mouse = Config::input().mouse;
    double delta = event->delta;
    int32_t delta_discrete = event->delta_discrete;
    if (mouse.natural_scroll) {
        delta = -delta;
        delta_discrete = -delta_discrete;
    }
    if (mouse.scroll_step_size > 1) {
        delta *= mouse.scroll_step_size;
        delta_discrete *= static_cast<int32_t>(mouse.scroll_step_size);
    }
    wlr_seat_pointer_notify_axis(server->seat, event->time_msec, event->orientation, delta, delta_discrete, event->source, event->relative_direction);
    notify_activity(*server);
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

// --- input device configuration (R4.6) ------------------------------------

// Build the xkb keymap from the configured layouts/variant/model/options and
// apply it (with repeat info) to every keyboard, falling back to the default
// keymap when the configuration does not compile.
static void apply_keyboard_input(Server& server)
{
    auto const& settings = Config::input().keyboard;
    auto model = settings.model;
    auto layout = settings.layouts;
    auto variant = settings.variant;
    auto options = settings.options;
    xkb_rule_names names {
        .rules = nullptr,
        .model = model.is_empty() ? nullptr : model.characters(),
        .layout = layout.is_empty() ? nullptr : layout.characters(),
        .variant = variant.is_empty() ? nullptr : variant.characters(),
        .options = options.is_empty() ? nullptr : options.characters(),
    };
    auto* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    auto* keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!keymap) {
        warnln("WindowServer: could not compile keymap for layouts '{}', using the default", settings.layouts);
        keymap = xkb_keymap_new_from_names(context, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    if (keymap) {
        for (auto& keyboard : server.keyboards)
            wlr_keyboard_set_keymap(keyboard->keyboard, keymap);
        xkb_keymap_unref(keymap);
    }
    xkb_context_unref(context);
    for (auto& keyboard : server.keyboards)
        wlr_keyboard_set_repeat_info(keyboard->keyboard, settings.repeat_rate, settings.repeat_delay);
}

// Apply the mouse settings to every libinput pointer: acceleration profile and
// speed, tap-to-click, natural scroll, left-handed and disable-while-typing.
// Virtual/headless pointers have no libinput handle and are skipped.
static void apply_mouse_input(Server& server)
{
    auto const& mouse = Config::input().mouse;
    for (auto* device : server.pointer_devices) {
        if (!wlr_input_device_is_libinput(device))
            continue;
        auto* libinput_device = wlr_libinput_get_device_handle(device);
        if (!libinput_device)
            continue;
        if (libinput_device_config_accel_is_available(libinput_device)) {
            libinput_device_config_accel_set_speed(libinput_device, mouse.acceleration);
            auto profile = mouse.accel_profile == "flat"sv ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
            libinput_device_config_accel_set_profile(libinput_device, profile);
        }
        if (libinput_device_config_tap_get_finger_count(libinput_device) > 0)
            libinput_device_config_tap_set_enabled(libinput_device, mouse.tap_to_click ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
        if (libinput_device_config_scroll_has_natural_scroll(libinput_device))
            libinput_device_config_scroll_set_natural_scroll_enabled(libinput_device, mouse.natural_scroll);
        if (libinput_device_config_left_handed_is_available(libinput_device))
            libinput_device_config_left_handed_set(libinput_device, mouse.left_handed);
        if (libinput_device_config_dwt_is_available(libinput_device))
            libinput_device_config_dwt_set_enabled(libinput_device, mouse.disable_while_typing ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
    }
}

// Re-apply all input settings: at startup, on device hotplug and on input.ini
// change.
static void apply_input_settings(Server& server)
{
    apply_keyboard_input(server);
    apply_mouse_input(server);
    dbgln("WindowServer: applied input settings (accel {}, natural-scroll {}, buttons-switched {}, left-handed {}, layouts '{}')",
        Config::input().mouse.acceleration, Config::input().mouse.natural_scroll ? "on" : "off",
        Config::input().mouse.buttons_switched ? "on" : "off", Config::input().mouse.left_handed ? "on" : "off", Config::input().keyboard.layouts);
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

        keyboard->modifiers.notify = keyboard_modifiers;
        wl_signal_add(&keyboard->keyboard->events.modifiers, &keyboard->modifiers);
        keyboard->key.notify = keyboard_key;
        wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
        keyboard->destroy.notify = keyboard_destroy;
        wl_signal_add(&device->events.destroy, &keyboard->destroy);

        wlr_seat_set_keyboard(server->seat, keyboard->keyboard);
        server->keyboards.append(move(keyboard));
        // Keymap and repeat come from the input config (R4.6).
        apply_keyboard_input(*server);

        // If a window is already mapped but has no keyboard focus (e.g. the first
        // keyboard just appeared), focus the topmost one so keybindings act on it.
        if (!server->seat->keyboard_state.focused_surface) {
            for (auto it = server->toplevels.rbegin(); it != server->toplevels.rend(); ++it) {
                if (!(*it)->minimized) {
                    focus_toplevel(*server, it->ptr());
                    break;
                }
            }
        }
        break;
    }
    case WLR_INPUT_DEVICE_POINTER:
        wlr_cursor_attach_input_device(server->cursor, device);
        server->pointer_devices.append(device);
        apply_mouse_input(*server);
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
// Logical size of an output (physical resolution divided by scale). The scene is
// laid out in logical coordinates, so anything we size or place ourselves must use
// this, not wlr_output->width/height (which are physical pixels).
static Gfx::IntSize output_logical_size(wlr_output* output)
{
    int width = 0;
    int height = 0;
    wlr_output_effective_resolution(output, &width, &height);
    return { width, height };
}

// Nearest filtering makes an integer upscale crisp (pixel-doubling) instead of
// bilinear-soft, and is a no-op at 1x. Applied to the app content and compositor
// chrome buffers so the HiDPI stopgap looks sharp.
static void set_buffer_nearest(struct wlr_scene_buffer* buffer, int, int, void*)
{
    wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_NEAREST);
}

static void use_nearest_filter(struct wlr_scene_node* node)
{
    wlr_scene_node_for_each_buffer(node, set_buffer_nearest, nullptr);
}

static void update_desktop_background(Server& server, Output& output)
{
    if (!g_palette)
        return;
    auto const logical = output_logical_size(output.output);
    int width = logical.width();
    int height = logical.height();
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
    use_nearest_filter(&output.background->node);
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
    wl_list_remove(&output->commit.link);
    if (output->background)
        wlr_scene_node_destroy(&output->background->node);
    remove_owned(output->server->outputs, output);
}

static void output_commit(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, commit);
    reconfigure_lock_surface(*output->server, output->output);
}

// --- HiDPI: integer output scale (stopgap) -------------------------------
//
// Scale a display by an integer factor (2x) on high-DPI panels and leave
// everything else at 1x. Serenity's LibGUI has no fractional-scale support and
// its fonts are fixed bitmaps, so an integer upscale of the 1x buffers is the
// pragmatic way to make a HiDPI panel usable. This adds no protocol surface
// (no wp-fractional-scale-v1, no backing-store scale); the real fix is the
// LibWindowServer M8 work. See docs/13-native-session-and-aur-packaging.md.
static float auto_output_scale(struct wlr_output* output, struct wlr_output_mode* mode)
{
    // Explicit override: SDE_OUTPUT_SCALE=1 disables, =2 forces.
    if (auto const* env = getenv("SDE_OUTPUT_SCALE"); env && *env) {
        char* end = nullptr;
        double const value = strtod(env, &end);
        if (end != env && value >= 1.0)
            return static_cast<float>(value);
    }

    // [Output] DefaultScale (R4.4): 0 = auto, else a fixed integer scale.
    if (int const configured = Config::output().default_scale; configured > 0)
        return static_cast<float>(configured);

    int const width = mode ? mode->width : output->width;
    int const height = mode ? mode->height : output->height;

    // Prefer the panel's physical size (true DPI) when it reports one...
    if (output->phys_width > 0) {
        double const dpi = width / (static_cast<double>(output->phys_width) / 25.4);
        return dpi >= 144.0 ? 2.0f : 1.0f;
    }
    // ...but eDP panels often omit it, so fall back to a resolution threshold
    // that keeps 1080p/1440p displays at 1x while catching 1600p/1800p/4K panels.
    return (width >= 2560 && height >= 1600) ? 2.0f : 1.0f;
}

static void server_new_output(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_output);
    auto* wlr_output = static_cast<struct wlr_output*>(data);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    // R4.5: honor a saved screen entry (mode/scale) for this output.
    Config::OutputSettings const* saved = nullptr;
    if (wlr_output->name) {
        for (auto const& settings : Config::outputs()) {
            if (settings.mode == "Device"sv && settings.device == wlr_output->name) {
                saved = &settings;
                break;
            }
        }
    }

    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    struct wlr_output_mode* mode = nullptr;
    if (saved && saved->width > 0 && saved->height > 0) {
        struct wlr_output_mode* candidate;
        wl_list_for_each(candidate, &wlr_output->modes, link)
        {
            if (candidate->width == saved->width && candidate->height == saved->height) {
                mode = candidate;
                break;
            }
        }
    }
    if (!mode)
        mode = wlr_output_preferred_mode(wlr_output);
    if (mode)
        wlr_output_state_set_mode(&state, mode);
    // HiDPI stopgap: integer-scale high-DPI panels (see auto_output_scale),
    // unless a saved screen layout specifies a scale.
    float scale = (saved && saved->scale > 0) ? static_cast<float>(saved->scale) : auto_output_scale(wlr_output, mode);
    // An explicit test/debug override (SDE_OUTPUT_SCALE) always wins over a
    // persisted layout.
    if (auto const* env = getenv("SDE_OUTPUT_SCALE"); env && *env) {
        char* end = nullptr;
        double const value = strtod(env, &end);
        if (end != env && value >= 1.0)
            scale = static_cast<float>(value);
    }
    if (scale != 1.0f) {
        wlr_output_state_set_scale(&state, scale);
        dbgln("WindowServer: output '{}' scaling {}x ({})",
            wlr_output->name ? wlr_output->name : "?", static_cast<int>(scale), saved ? "saved layout" : "high-DPI");
    } else if (saved) {
        dbgln("WindowServer: output '{}' scale 1x (saved layout)", wlr_output->name ? wlr_output->name : "?");
    }
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
    output->commit.notify = output_commit;
    wl_signal_add(&wlr_output->events.commit, &output->commit);

    auto* layout_output = saved
        ? wlr_output_layout_add(server->output_layout, wlr_output, saved->left, saved->top)
        : wlr_output_layout_add_auto(server->output_layout, wlr_output);
    auto* scene_output = wlr_scene_output_create(server->scene, wlr_output);
    wlr_scene_output_layout_add_output(server->scene_layout, layout_output, scene_output);

    if (!server->primary_output)
        server->primary_output = wlr_output;

    auto const logical_size = output_logical_size(wlr_output);
    dbgln("WindowServer: output '{}' {}x{} @{}x", wlr_output->name ? wlr_output->name : "?", logical_size.width(), logical_size.height(), static_cast<int>(wlr_output->scale));
    server->outputs.append(move(output));
    update_desktop_background(*server, *server->outputs.last());
    arrange_layers(*server, wlr_output);
    output_manager_send_configuration(*server);
    // R4.5: keep screenlayout.ini in sync with the live layout.
    persist_screen_layout(*server);
}

// --- xdg toplevels / popups ----------------------------------------------

static void toplevel_report_position(Toplevel& toplevel)
{
    // Store session geometry (R4.2), independent of whether the client has a
    // chrome handle.
    SessionManager::note_geometry(toplevel);
    if (!toplevel.chrome)
        return;
    // Report the *content* origin, not the frame origin: the client's coordinate
    // space starts at its surface (which the compositor offsets by the frame
    // inset), so screen-relative coordinates (tooltips, widget rects) must be
    // built from the content top-left.
    int content_x = static_cast<int>(toplevel.scene_tree->node.x) + static_cast<int>(toplevel.content_tree->node.x);
    int content_y = static_cast<int>(toplevel.scene_tree->node.y) + static_cast<int>(toplevel.content_tree->node.y);
    serenity_toplevel_send_position(toplevel.chrome, content_x, content_y);
}

// Report a layer surface's (an applet's) real output position. The compositor
// moves applets into the Taskbar's applet area, away from their anchored corner,
// so the client cannot know its own origin without this.
static void layer_report_position(LayerSurface& layer)
{
    if (!layer.chrome || !layer.scene)
        return;
    serenity_toplevel_send_position(layer.chrome,
        static_cast<int>(layer.scene->tree->node.x), static_cast<int>(layer.scene->tree->node.y));
}

static void toplevel_update_ext_foreign(Toplevel& toplevel)
{
    if (!toplevel.ext_foreign_handle)
        return;
    wlr_ext_foreign_toplevel_handle_v1_state state {
        .title = toplevel.xdg_toplevel->title ? toplevel.xdg_toplevel->title : "",
        .app_id = toplevel.xdg_toplevel->app_id ? toplevel.xdg_toplevel->app_id : "",
    };
    wlr_ext_foreign_toplevel_handle_v1_update_state(toplevel.ext_foreign_handle, &state);
}

static void toplevel_update_foreign(Toplevel& toplevel)
{
    if (toplevel.foreign_handle) {
        wlr_foreign_toplevel_handle_v1_set_title(toplevel.foreign_handle, toplevel.xdg_toplevel->title ? toplevel.xdg_toplevel->title : "");
        wlr_foreign_toplevel_handle_v1_set_app_id(toplevel.foreign_handle, toplevel.xdg_toplevel->app_id ? toplevel.xdg_toplevel->app_id : "");
    }
    toplevel_update_ext_foreign(toplevel);
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
    use_nearest_filter(&toplevel.frame_buffer->node);
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
    // The new window joins the current workspace stack.
    WindowManager::on_toplevel_mapped(*toplevel);
    // Apply a restored session position (R4.2), then report the final position.
    SessionManager::on_toplevel_mapped(*toplevel);
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
    if (toplevel->xdg_toplevel->base->initial_commit) {
        // Restore the stored session size (R4.2), if any; otherwise let the
        // client pick.
        if (toplevel->has_session_restore_size) {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, toplevel->session_restore_size.width(), toplevel->session_restore_size.height());
            toplevel->has_session_restore_size = false;
        } else {
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
        }
    }
    // A decoration created before the surface was initialized is configured now.
    if (toplevel->decoration && toplevel->xdg_toplevel->base->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(toplevel->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        toplevel->decoration = nullptr;
    }
    toplevel_update_foreign(*toplevel);
    toplevel_update_frame(*toplevel);
    maybe_dump_thumbnail(*toplevel);
    WindowManager::emit_window_state(*toplevel);
    // Keep the window switcher's live thumbnails current while it is open (WS3).
    if (WindowSwitcher::is_visible())
        WindowSwitcher::refresh(*toplevel->server);
}

static void toplevel_destroy(wl_listener* listener, void*)
{
    Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, destroy);
    auto& server = *toplevel->server;
    SessionManager::on_toplevel_destroyed(*toplevel);
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
    if (toplevel->ext_foreign_handle)
        wlr_ext_foreign_toplevel_handle_v1_destroy(toplevel->ext_foreign_handle);
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
                && (time - server.last_titlebar_press_time) < static_cast<uint32_t>(Config::input().mouse.double_click_speed)
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
    use_nearest_filter(&toplevel->content_tree->node);

    if (server->foreign_toplevel_manager) {
        toplevel->foreign_handle = wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
        toplevel->foreign_handle->data = toplevel.ptr();
        toplevel->foreign_request_activate.notify = toplevel_foreign_request_activate;
        wl_signal_add(&toplevel->foreign_handle->events.request_activate, &toplevel->foreign_request_activate);
        toplevel->foreign_request_close.notify = toplevel_foreign_request_close;
        wl_signal_add(&toplevel->foreign_handle->events.request_close, &toplevel->foreign_request_close);
    }

    if (server->ext_foreign_toplevel_list) {
        wlr_ext_foreign_toplevel_handle_v1_state state {
            .title = xdg_toplevel->title ? xdg_toplevel->title : "",
            .app_id = xdg_toplevel->app_id ? xdg_toplevel->app_id : "",
        };
        toplevel->ext_foreign_handle = wlr_ext_foreign_toplevel_handle_v1_create(server->ext_foreign_toplevel_list, &state);
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
    auto* popup_tree = wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
    xdg_popup->base->data = popup_tree;
    use_nearest_filter(&popup_tree->node);

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

// The primary output's usable area (the window-manager tiling target, WS2).
static Gfx::IntRect primary_usable_area(Server& server)
{
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &box);
    if (auto* output_state = output_for(server, server.primary_output); output_state && !wlr_box_empty(&output_state->usable_area))
        box = output_state->usable_area;
    return { box.x, box.y, box.width, box.height };
}

// The current theme palette, for the window switcher (WS3).
static Gfx::Palette const& current_palette(Server&)
{
    VERIFY(g_palette);
    return *g_palette;
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

    // The layer-shell arrange resets every node's position, so re-apply the
    // applet-area layout last (WS4).
    AppletManager::relayout(server);
}

static void layer_surface_commit(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, commit);

    // Only re-arrange when the committed state can actually change the layout.
    // Layer-shell reconfigures every surface on commit; doing that for a
    // content-only commit re-configures every layer surface and spins any client
    // that commits in response to a configure (see docs/04-known-gaps.md).
    auto const& state = layer->layer_surface->current;
    auto const& previous = layer->arrange_state;
    bool const arrange_relevant = !previous.valid
        || previous.desired_width != state.desired_width
        || previous.desired_height != state.desired_height
        || previous.anchor != state.anchor
        || previous.exclusive_zone != state.exclusive_zone
        || previous.margin_top != state.margin.top
        || previous.margin_right != state.margin.right
        || previous.margin_bottom != state.margin.bottom
        || previous.margin_left != state.margin.left
        || previous.layer != static_cast<uint32_t>(state.layer);
    if (!arrange_relevant)
        return;

    layer->arrange_state.valid = true;
    layer->arrange_state.desired_width = state.desired_width;
    layer->arrange_state.desired_height = state.desired_height;
    layer->arrange_state.anchor = state.anchor;
    layer->arrange_state.exclusive_zone = state.exclusive_zone;
    layer->arrange_state.margin_top = state.margin.top;
    layer->arrange_state.margin_right = state.margin.right;
    layer->arrange_state.margin_bottom = state.margin.bottom;
    layer->arrange_state.margin_left = state.margin.left;
    layer->arrange_state.layer = static_cast<uint32_t>(state.layer);

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
    layer->is_applet = AppletManager::parse_namespace(layer_surface->layer_namespace, layer->applet_name);
    char const* layer_name = "unknown";
    wlr_scene_tree* layer_parent = server->background_tree;
    switch (layer_surface->pending.layer) {
    case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
        layer_parent = server->background_tree;
        layer_name = "background";
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
        layer_parent = server->bottom_tree;
        layer_name = "bottom";
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
        layer_parent = server->top_tree;
        layer_name = "top";
        break;
    case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
        layer_parent = server->overlay_tree;
        layer_name = "overlay";
        break;
    default:
        break;
    }
    dbgln("WindowServer: layer surface namespace='{}' layer={}", layer_surface->layer_namespace ? layer_surface->layer_namespace : "", layer_name);
    layer->scene = wlr_scene_layer_surface_v1_create(layer_parent, layer_surface);
    layer->scene->tree->node.data = layer.ptr();
    use_nearest_filter(&layer->scene->tree->node);
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

// R4.5: persist the live output layout to screenlayout.ini so it survives a
// restart and is visible to DisplaySettings (via the port's get_screen_layout).
static void persist_screen_layout(Server& server)
{
    Vector<Config::OutputSettings> settings;
    unsigned main_index = 0;
    for (size_t index = 0; index < server.outputs.size(); ++index) {
        auto& output = *server.outputs[index];
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, output.output, &box);
        Config::OutputSettings entry;
        entry.mode = "Device"sv;
        entry.device = output.output->name ? output.output->name : "sde-0";
        entry.left = box.x;
        entry.top = box.y;
        entry.width = output.output->width;
        entry.height = output.output->height;
        entry.scale = static_cast<int>(output.output->scale);
        settings.append(move(entry));
        if (output.output == server.primary_output)
            main_index = index;
    }
    Config::set_outputs(move(settings), main_index);
}

// R4.5: apply a saved screen entry (mode/scale/position) to a live output.
static void apply_saved_settings_to_output(Server& server, struct wlr_output* output, Config::OutputSettings const& settings)
{
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);

    struct wlr_output_mode* mode = nullptr;
    if (settings.width > 0 && settings.height > 0) {
        struct wlr_output_mode* candidate;
        wl_list_for_each(candidate, &output->modes, link)
        {
            if (candidate->width == settings.width && candidate->height == settings.height) {
                mode = candidate;
                break;
            }
        }
    }
    if (!mode)
        mode = wlr_output_preferred_mode(output);
    if (mode)
        wlr_output_state_set_mode(&state, mode);
    if (settings.scale > 0)
        wlr_output_state_set_scale(&state, static_cast<float>(settings.scale));
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    wlr_output_layout_add(server.output_layout, output, settings.left, settings.top);
    dbgln("WindowServer: applied screen layout to '{}': {}x{} @{}x at {},{}",
        output->name ? output->name : "?", settings.width, settings.height, settings.scale, settings.left, settings.top);
}

// R4.5: re-apply the on-disk layout to the live outputs (screenlayout.ini watch).
static void apply_loaded_screen_layout(Server& server)
{
    bool changed = false;
    for (auto const& settings : Config::outputs()) {
        if (settings.mode != "Device"sv || settings.device.is_empty())
            continue;
        for (auto& output : server.outputs) {
            if (output->output->name && settings.device == output->output->name) {
                apply_saved_settings_to_output(server, output->output, settings);
                changed = true;
                break;
            }
        }
    }
    if (changed)
        output_manager_send_configuration(server);
}

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

    if (!test) {
        output_manager_send_configuration(server);
        // R4.5: a monitor-layout change via zwlr_output_management_v1 is persisted.
        persist_screen_layout(server);
    }
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

// User data for a serenity_toplevel chrome handle: it is attached to either an
// xdg toplevel (menubar visibility, position) or a layer surface/applet
// (position), so the two share one resource type.
struct SerenityChrome {
    Toplevel* toplevel { nullptr };
    LayerSurface* layer { nullptr };
};

static void serenity_toplevel_resource_destroy(wl_resource* resource)
{
    auto* chrome = static_cast<SerenityChrome*>(wl_resource_get_user_data(resource));
    if (!chrome)
        return;
    if (chrome->toplevel && chrome->toplevel->chrome == resource)
        chrome->toplevel->chrome = nullptr;
    if (chrome->layer && chrome->layer->chrome == resource)
        chrome->layer->chrome = nullptr;
    delete chrome;
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

    // Layer surfaces (the applet area) also get a chrome handle, so the compositor
    // can report where it actually put them.
    LayerSurface* layer = nullptr;
    if (!toplevel) {
        for (auto& candidate : server.layer_surfaces) {
            if (candidate->layer_surface && candidate->layer_surface->surface == wlr_surface) {
                layer = candidate.ptr();
                break;
            }
        }
    }
    if (!toplevel && !layer) {
        wl_resource_post_error(resource, WL_DISPLAY_ERROR_INVALID_OBJECT, "unknown wl_surface");
        return;
    }

    auto* chrome = wl_resource_create(client, &serenity_toplevel_interface, 1, id);
    if (!chrome) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(chrome, &s_serenity_toplevel_implementation, new SerenityChrome { toplevel, layer }, serenity_toplevel_resource_destroy);

    if (toplevel) {
        toplevel->chrome = chrome;
        // Bring the client up to date with the current menubar visibility.
        if (!toplevel->menubar_visible)
            serenity_toplevel_send_menubar_visibility(chrome, 0);
        toplevel_report_position(*toplevel);
    } else if (layer) {
        layer->chrome = chrome;
        layer_report_position(*layer);
    }
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

// --- session safety net (R4.1) --------------------------------------------
// Idle notification (ext-idle-notify-v1), idle inhibition (zwp_idle_inhibit_v1)
// and DPMS (wlr-output-power-management). See docs/11-r4-implementation-plan.md.

static void notify_activity(Server& server)
{
    if (server.idle_notifier)
        wlr_idle_notifier_v1_notify_activity(server.idle_notifier, server.seat);
}

static void update_idle_inhibit(Server& server, wlr_idle_inhibitor_v1* being_destroyed = nullptr)
{
    if (!server.idle_notifier || !server.idle_inhibit_manager)
        return;
    // wlroots removes an inhibitor from the manager's list *after* emitting its
    // destroy signal, so when we are called from that signal the dying inhibitor
    // is still linked; exclude it or we would never clear the inhibited state.
    bool inhibited = false;
    wlr_idle_inhibitor_v1* inhibitor = nullptr;
    wl_list_for_each(inhibitor, &server.idle_inhibit_manager->inhibitors, link)
    {
        if (inhibitor == being_destroyed)
            continue;
        inhibited = true;
        break;
    }
    wlr_idle_notifier_v1_set_inhibited(server.idle_notifier, inhibited);
}

static void idle_inhibitor_destroy(wl_listener* listener, void*)
{
    IdleInhibitor* inhibitor = wl_container_of(listener, inhibitor, destroy);
    auto& server = *inhibitor->server;
    dbgln("WindowServer: idle inhibitor destroyed");
    // Unlink before freeing the entry: wlroots asserts the inhibitor's destroy
    // listener list is empty before it frees the object.
    wl_list_remove(&inhibitor->destroy.link);
    auto* dying = inhibitor->inhibitor;
    server.idle_inhibitors.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == inhibitor; });
    update_idle_inhibit(server, dying);
}

static void server_new_idle_inhibitor(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_idle_inhibitor);
    auto* inhibitor = static_cast<wlr_idle_inhibitor_v1*>(data);
    dbgln("WindowServer: idle inhibitor created");

    auto entry = make<IdleInhibitor>();
    entry->server = server;
    entry->inhibitor = inhibitor;
    entry->destroy.notify = idle_inhibitor_destroy;
    wl_signal_add(&inhibitor->events.destroy, &entry->destroy);
    server->idle_inhibitors.append(move(entry));
    update_idle_inhibit(*server);
}

static void set_output_power(wlr_output* output, bool on)
{
    wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, on);
    if (!wlr_output_commit_state(output, &state)) {
        dbgln("WindowServer: failed to turn output '{}' {}", output->name ? output->name : "?", on ? "on" : "off");
    } else {
        dbgln("WindowServer: output '{}' {}", output->name ? output->name : "?", on ? "on" : "off");
    }
    wlr_output_state_finish(&state);
}

static void output_power_set_mode(wl_listener* listener, void* data)
{
    (void)listener;
    auto* event = static_cast<wlr_output_power_v1_set_mode_event*>(data);
    set_output_power(event->output, event->mode == ZWLR_OUTPUT_POWER_V1_MODE_ON);
}

// --- ext-session-lock-v1 ---------------------------------------------------
// The compositor must cover every output with a configured lock surface before
// telling the client the session is locked, and must not deliver input to normal
// clients while locked.

static void spawn_lock_client()
{
    // A standard ext-session-lock-v1 client, so any conformant locker works.
    // swaylock is the default; override with SDE_LOCK_CMD (gtklock, hyprlock, a
    // future SDE-native client, ...).
    char const* command = getenv("SDE_LOCK_CMD");
    if (!command || !*command)
        command = "swaylock";
    dbgln("WindowServer: locking via '{}'", command);
    if (fork() == 0) {
        execl("/bin/sh", "/bin/sh", "-c", command, static_cast<char*>(nullptr));
        _exit(127);
    }
}

static LockSurface* lock_surface_for_output(Server& server, wlr_output* output)
{
    for (auto& lock : server.lock_surfaces) {
        if (lock->lock_surface && lock->lock_surface->output == output && lock->lock_surface->surface)
            return lock.ptr();
    }
    return nullptr;
}

static LockSurface* lock_surface_at_cursor(Server& server)
{
    // Prefer the lock surface on the output under the cursor; fall back to any
    // (e.g. a headless/cursorless client).
    auto* output = wlr_output_layout_output_at(server.output_layout, server.cursor->x, server.cursor->y);
    if (auto* lock = lock_surface_for_output(server, output))
        return lock;
    for (auto& lock : server.lock_surfaces) {
        if (lock->lock_surface && lock->lock_surface->surface)
            return lock.ptr();
    }
    return nullptr;
}

static void reconfigure_lock_surface(Server& server, wlr_output* output)
{
    if (!server.locked || !output)
        return;
    auto* lock = lock_surface_for_output(server, output);
    if (!lock)
        return;
    // output.commit fires every frame; only reconfigure on a real size change.
    Gfx::IntSize const wanted = output_logical_size(output);
    if (lock->configured_size == wanted)
        return;
    lock->configured_size = wanted;
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, output, &box);
    dbgln("WindowServer: lock surface re-configured for '{}' {}x{}", output->name ? output->name : "?", wanted.width(), wanted.height());
    if (lock->scene)
        wlr_scene_node_set_position(&lock->scene->buffer->node, box.x, box.y);
    wlr_session_lock_surface_v1_configure(lock->lock_surface, static_cast<uint32_t>(wanted.width()), static_cast<uint32_t>(wanted.height()));
}

static void lock_surface_commit(wl_listener* listener, void*)
{
    LockSurface* lock = wl_container_of(listener, lock, commit);
    Server& server = *lock->server;
    if (lock->configured)
        return;
    if (!lock->lock_surface || !lock->lock_surface->surface || !lock->lock_surface->surface->buffer)
        return;

    lock->configured = true;
    dbgln("WindowServer: session lock surface covered an output");

    int covered = 0;
    for (auto& candidate : server.lock_surfaces)
        covered += candidate->configured ? 1 : 0;
    if (server.session_lock && !server.locked && covered >= static_cast<int>(server.outputs.size())) {
        wlr_session_lock_v1_send_locked(server.session_lock);
        server.locked = true;
        // Give the keyboard to a lock surface (the primary output's, else any).
        auto* lock = lock_surface_for_output(server, server.primary_output);
        if (!lock)
            lock = lock_surface_at_cursor(server);
        if (lock) {
            if (auto* keyboard = wlr_seat_get_keyboard(server.seat))
                wlr_seat_keyboard_notify_enter(server.seat, lock->lock_surface->surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
        }
        dbgln("WindowServer: session locked");
    }
}

static void lock_surface_destroy(wl_listener* listener, void*)
{
    LockSurface* lock = wl_container_of(listener, lock, destroy);
    Server& server = *lock->server;
    wl_list_remove(&lock->commit.link);
    wl_list_remove(&lock->destroy.link);
    if (lock->scene)
        wlr_scene_node_destroy(&lock->scene->buffer->node);
    server.lock_surfaces.remove_first_matching([&](auto const& candidate) { return candidate.ptr() == lock; });
}

static void session_lock_new_surface(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, session_lock_new_surface);
    auto* lock_surface = static_cast<wlr_session_lock_surface_v1*>(data);
    auto* output = lock_surface->output;
    dbgln("WindowServer: session lock surface for output '{}'", output && output->name ? output->name : "?");

    auto entry = make<LockSurface>();
    entry->server = server;
    entry->lock_surface = lock_surface;
    entry->scene = wlr_scene_surface_create(server->overlay_tree, lock_surface->surface);
    use_nearest_filter(&entry->scene->buffer->node);
    wlr_box box { };
    if (output)
        wlr_output_layout_get_box(server->output_layout, output, &box);
    wlr_scene_node_set_position(&entry->scene->buffer->node, box.x, box.y);
    wlr_scene_node_set_enabled(&entry->scene->buffer->node, true);

    entry->commit.notify = lock_surface_commit;
    wl_signal_add(&lock_surface->surface->events.commit, &entry->commit);
    entry->destroy.notify = lock_surface_destroy;
    wl_signal_add(&lock_surface->events.destroy, &entry->destroy);

    if (output) {
        auto const wanted = output_logical_size(output);
        entry->configured_size = wanted;
        wlr_session_lock_surface_v1_configure(lock_surface, static_cast<uint32_t>(wanted.width()), static_cast<uint32_t>(wanted.height()));
    }

    server->lock_surfaces.append(move(entry));
}

static void session_lock_unlock(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, session_lock_unlock);
    dbgln("WindowServer: session unlocked");
    // wlroots destroys the lock surfaces (and our per-surface handlers clean them
    // up) as the client destroys the lock right after this signal.
    server->locked = false;
}

static void session_lock_destroy(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, session_lock_destroy);
    // By now wlroots has destroyed every lock surface (their handlers removed
    // them). Unlink our lock listeners so wlroots' final assertions pass.
    wl_list_remove(&server->session_lock_new_surface.link);
    wl_list_remove(&server->session_lock_unlock.link);
    wl_list_remove(&server->session_lock_destroy.link);
    server->session_lock = nullptr;
    server->locked = false;
}

static void session_lock_new_lock(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, session_lock_new_lock);
    auto* lock = static_cast<wlr_session_lock_v1*>(data);
    if (server->session_lock) {
        wlr_session_lock_v1_destroy(lock);
        return;
    }
    dbgln("WindowServer: session lock requested");
    server->session_lock = lock;
    server->session_lock_new_surface.notify = session_lock_new_surface;
    wl_signal_add(&lock->events.new_surface, &server->session_lock_new_surface);
    server->session_lock_unlock.notify = session_lock_unlock;
    wl_signal_add(&lock->events.unlock, &server->session_lock_unlock);
    server->session_lock_destroy.notify = session_lock_destroy;
    wl_signal_add(&lock->events.destroy, &server->session_lock_destroy);
}

// --- setup ----------------------------------------------------------------

// --- R4.8: standard interop (ext-workspace-v1, xdg-activation-v1) ----------

// Publish the SDE workspace grid over ext-workspace-v1: one group (the outputs)
// and one workspace per grid cell, with grid coordinates and the active state.
static void rebuild_ext_workspaces(Server& server)
{
    for (auto* workspace : server.ext_workspaces)
        wlr_ext_workspace_handle_v1_destroy(workspace);
    server.ext_workspaces.clear();
    if (!server.ext_workspace_manager)
        return;

    if (!server.ext_workspace_group) {
        server.ext_workspace_group = wlr_ext_workspace_group_handle_v1_create(server.ext_workspace_manager, 0);
        for (auto& output : server.outputs)
            wlr_ext_workspace_group_handle_v1_output_enter(server.ext_workspace_group, output->output);
    }

    u32 rows = WindowManager::workspace_rows();
    u32 columns = WindowManager::workspace_columns();
    u32 current_row = WindowManager::current_workspace_row();
    u32 current_column = WindowManager::current_workspace_column();
    for (u32 row = 0; row < rows; ++row) {
        for (u32 column = 0; column < columns; ++column) {
            auto id = ByteString::formatted("{},{}", row, column);
            auto* workspace = wlr_ext_workspace_handle_v1_create(server.ext_workspace_manager, id.characters(), EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE);
            auto name = ByteString::formatted("{}", row * columns + column + 1);
            wlr_ext_workspace_handle_v1_set_name(workspace, name.characters());
            uint32_t coordinates[2] = { column, row };
            wlr_ext_workspace_handle_v1_set_coordinates(workspace, coordinates, 2);
            wlr_ext_workspace_handle_v1_set_group(workspace, server.ext_workspace_group);
            wlr_ext_workspace_handle_v1_set_active(workspace, row == current_row && column == current_column);
            server.ext_workspaces.append(workspace);
        }
    }
    dbgln("WindowServer: ext-workspace-v1 published {}x{} grid", rows, columns);
}

static void update_ext_workspace_active(Server& server)
{
    if (!server.ext_workspace_manager)
        return;
    u32 columns = max(1u, WindowManager::workspace_columns());
    if (server.ext_workspaces.size() != WindowManager::workspace_rows() * columns) {
        rebuild_ext_workspaces(server);
        return;
    }
    u32 current_row = WindowManager::current_workspace_row();
    u32 current_column = WindowManager::current_workspace_column();
    for (size_t index = 0; index < server.ext_workspaces.size(); ++index) {
        u32 row = static_cast<u32>(index) / columns;
        u32 column = static_cast<u32>(index) % columns;
        wlr_ext_workspace_handle_v1_set_active(server.ext_workspaces[index], row == current_row && column == current_column);
    }
}

static void ext_workspace_commit(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, ext_workspace_commit);
    auto* event = static_cast<wlr_ext_workspace_v1_commit_event*>(data);
    wlr_ext_workspace_v1_request* request = nullptr;
    wl_list_for_each(request, event->requests, link)
    {
        if (request->type != WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE || !request->activate.workspace)
            continue;
        auto* workspace = request->activate.workspace;
        if (workspace->coordinates.size < 2 * sizeof(uint32_t))
            continue;
        auto const* coordinates = static_cast<uint32_t const*>(workspace->coordinates.data);
        dbgln("WindowServer: ext-workspace-v1 activate {},{}", coordinates[1], coordinates[0]);
        WindowManager::switch_workspace(*server, coordinates[1], coordinates[0], false);
    }
}

// xdg-activation-v1: a client asked to be focused. Switch to its workspace and
// focus it.
static void xdg_activation_activate(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, xdg_activation_activate);
    auto* event = static_cast<wlr_xdg_activation_v1_request_activate_event*>(data);
    for (auto& toplevel : server->toplevels) {
        if (!toplevel->xdg_toplevel || toplevel->xdg_toplevel->base->surface != event->surface)
            continue;
        WindowManager::switch_workspace(*server, toplevel->workspace_row, toplevel->workspace_column, false);
        focus_toplevel(*server, toplevel.ptr());
        dbgln("WindowServer: xdg-activation focused '{}'", toplevel->xdg_toplevel->title ? toplevel->xdg_toplevel->title : "");
        break;
    }
}

// wp-cursor-shape-v1: a client asked for a named cursor shape (e.g. "text").
static void cursor_shape_request(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_shape_request);
    auto* event = static_cast<wlr_cursor_shape_manager_v1_request_set_shape_event*>(data);
    if (event->device_type != WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER)
        return;
    if (auto const* name = wlr_cursor_shape_v1_name(event->shape); name && server->cursor)
        wlr_cursor_set_xcursor(server->cursor, server->cursor_manager, name);
}

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
    SessionManager::initialize(server);

    // R4.8: the standard interop surface. ext-workspace-v1 maps the SDE grid;
    // ext-foreign-toplevel-list-v1 maps the window list; xdg-activation-v1 lets a
    // client ask to be focused.
    server.ext_workspace_manager = wlr_ext_workspace_manager_v1_create(server.display, 1);
    server.ext_workspace_commit.notify = ext_workspace_commit;
    wl_signal_add(&server.ext_workspace_manager->events.commit, &server.ext_workspace_commit);
    server.ext_foreign_toplevel_list = wlr_ext_foreign_toplevel_list_v1_create(server.display, 1);
    // Window capture sources (ext-image-capture-source-v1) for the ScreenCast portal.
    server.ext_toplevel_capture_source_manager = wlr_ext_foreign_toplevel_image_capture_source_manager_v1_create(server.display, 1);
    server.xdg_activation = wlr_xdg_activation_v1_create(server.display);
    server.xdg_activation_activate.notify = xdg_activation_activate;
    wl_signal_add(&server.xdg_activation->events.request_activate, &server.xdg_activation_activate);

    // wp-cursor-shape-v1: clients can set named cursor shapes (R4.7/R4.8).
    server.cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server.display, 1);
    server.cursor_shape_request.notify = cursor_shape_request;
    wl_signal_add(&server.cursor_shape_manager->events.request_set_shape, &server.cursor_shape_request);

    // Screen capture. Expose the standard ext-image-copy-capture-v1 (the primary,
    // portable capture surface) and the wlr-screencopy-unstable-v1 compat so
    // standard tools like `grim` work unmodified. See docs/03 D7.
    (void)wlr_ext_output_image_capture_source_manager_v1_create(server.display, 1);
    (void)wlr_ext_image_copy_capture_manager_v1_create(server.display, 1);
    (void)wlr_screencopy_manager_v1_create(server.display);

    server.output_manager = wlr_output_manager_v1_create(server.display);
    server.output_manager_apply.notify = output_manager_apply;
    wl_signal_add(&server.output_manager->events.apply, &server.output_manager_apply);
    server.output_manager_test.notify = output_manager_test;
    wl_signal_add(&server.output_manager->events.test, &server.output_manager_test);

    // Session safety net (R4.1): idle notification, idle inhibition and DPMS.
    server.idle_notifier = wlr_idle_notifier_v1_create(server.display);
    server.idle_inhibit_manager = wlr_idle_inhibit_v1_create(server.display);
    server.new_idle_inhibitor.notify = server_new_idle_inhibitor;
    wl_signal_add(&server.idle_inhibit_manager->events.new_inhibitor, &server.new_idle_inhibitor);
    server.output_power_manager = wlr_output_power_manager_v1_create(server.display);
    server.output_power_set_mode.notify = output_power_set_mode;
    wl_signal_add(&server.output_power_manager->events.set_mode, &server.output_power_set_mode);
    server.session_lock_manager = wlr_session_lock_manager_v1_create(server.display);
    server.session_lock_new_lock.notify = session_lock_new_lock;
    wl_signal_add(&server.session_lock_manager->events.new_lock, &server.session_lock_new_lock);

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
    // R4.5: the persisted output layout (screenlayout.ini) is applied as outputs
    // are created (server_new_output).
    Config::load_outputs();
    // R4.6: input device settings (input.ini) are applied as devices are created
    // (server_new_input).
    Config::load_input();
    // R5: global shortcuts requested by apps (global-shortcuts.ini).
    Config::load_global_shortcuts();

    // R4.7: a persisted font choice ([Fonts]) overrides the Katica default.
    if (!Config::fonts().default_query.is_empty()) {
        Gfx::FontDatabase::set_default_font_query(Config::fonts().default_query);
        if (!Config::fonts().fixed_width_query.is_empty())
            Gfx::FontDatabase::set_fixed_width_font_query(Config::fonts().fixed_width_query);
        if (!Config::fonts().window_title_query.is_empty())
            Gfx::FontDatabase::set_window_title_font_query(Config::fonts().window_title_query);
        dbgln("WindowServer: applied persisted system fonts (default '{}')", Config::fonts().default_query);
    }

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
        if (!theme_path.is_empty()) {
            apply_theme_path(server, theme_path);
            // Persist the choice so it survives a compositor/session restart.
            Config::set_theme({ LexicalPath(theme_path).title(), theme_path });
        }
    };
    if (auto directory = LexicalPath(path).dirname(); !directory.is_empty())
        (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    // Ensure the file exists so we can watch its content; a write is IN_MODIFY
    // (ContentModified), which a directory watch does not report.
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}

// R4.7: system fonts. The session font broker ($XDG_RUNTIME_DIR/sde-fonts) holds
// three lines (default / fixed-width / window-title queries); the compositor
// applies them to its own chrome and persists the choice to [Fonts].
static ByteString fonts_broker_path()
{
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-fonts", runtime);
    return ByteString { "/tmp/sde-fonts" };
}

static void apply_font_queries(Server& server, ByteString const& default_query, ByteString const& fixed_width_query, ByteString const& window_title_query)
{
    if (default_query.is_empty())
        return;
    Gfx::FontDatabase::set_default_font_query(default_query);
    if (!fixed_width_query.is_empty())
        Gfx::FontDatabase::set_fixed_width_font_query(fixed_width_query);
    if (!window_title_query.is_empty())
        Gfx::FontDatabase::set_window_title_font_query(window_title_query);
    for (auto& toplevel : server.toplevels)
        toplevel_update_frame(*toplevel);
    dbgln("WindowServer: applied system fonts (default '{}')", default_query);
}

static void read_and_apply_fonts(Server& server)
{
    auto file = Core::File::open(fonts_broker_path(), Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return;
    auto lines = StringView { contents.value().bytes() }.split_view('\n');
    ByteString default_query, fixed_width_query, window_title_query;
    if (lines.size() > 0)
        default_query = lines[0].trim_whitespace().to_byte_string();
    if (lines.size() > 1)
        fixed_width_query = lines[1].trim_whitespace().to_byte_string();
    if (lines.size() > 2)
        window_title_query = lines[2].trim_whitespace().to_byte_string();
    apply_font_queries(server, default_query, fixed_width_query, window_title_query);
    if (!default_query.is_empty())
        Config::set_fonts({ default_query, fixed_width_query, window_title_query });
}

static void watch_fonts_broker(Server& server)
{
    static RefPtr<Core::FileWatcher> watcher;
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto path = fonts_broker_path();
    watcher->on_change = [&server, path](Core::FileWatcherEvent const& event) {
        if (event.event_path != path)
            return;
        read_and_apply_fonts(server);
    };
    if (auto directory = LexicalPath(path).dirname(); !directory.is_empty())
        (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}

// R4.7: cursor settings. Mouse Settings' Theme tab writes cursor.ini; apply the
// chosen theme/size to the compositor's cursor manager and publish XCURSOR_* for
// clients. (Per-window cursor shapes via wp-cursor-shape-v1 and the highlight
// overlay are follow-ups.)
static ByteString cursor_state_path()
{
    return ByteString::formatted("{}/cursor.ini", Config::state_directory());
}

static void apply_cursor_state(Server& server)
{
    auto config = Core::ConfigFile::open(cursor_state_path());
    if (config.is_error())
        return;
    auto theme = config.value()->read_entry("Cursor"sv, "Theme"sv, "Default"sv);
    auto size = config.value()->read_num_entry<int>("Cursor"sv, "Size"sv, 24);
    auto size_string = ByteString::number(size);
    setenv("XCURSOR_THEME", theme.characters(), true);
    setenv("XCURSOR_SIZE", size_string.characters(), true);

    if (!theme.is_empty() && theme != "Default"sv) {
        // wlroots 0.20 has no set_theme: recreate the manager with the new theme.
        if (auto* manager = wlr_xcursor_manager_create(theme.characters(), size)) {
            for (auto& output : server.outputs)
                wlr_xcursor_manager_load(manager, output->output->scale);
            auto* old_manager = server.cursor_manager;
            server.cursor_manager = manager;
            if (server.cursor)
                wlr_cursor_set_xcursor(server.cursor, manager, "default");
            if (old_manager)
                wlr_xcursor_manager_destroy(old_manager);
            dbgln("WindowServer: applied cursor theme '{}' (size {})", theme, size);
        }
    }

    // Cursor highlight: a translucent ellipse behind the cursor (>= 20px enables it).
    g_cursor_highlight_radius = config.value()->read_num_entry<int>("Highlight"sv, "Radius"sv, 0);
    g_cursor_highlight_color = Gfx::Color::from_string(config.value()->read_entry("Highlight"sv, "Color"sv)).value_or(Gfx::Color::Transparent);
    render_cursor_highlight(server);
    if (g_cursor_highlight_radius >= 20)
        dbgln("WindowServer: cursor highlight radius {} color {}", g_cursor_highlight_radius, g_cursor_highlight_color.to_byte_string());
}

static void watch_cursor_state(Server& server)
{
    static RefPtr<Core::FileWatcher> watcher;
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto path = cursor_state_path();
    watcher->on_change = [&server, path](Core::FileWatcherEvent const& event) {
        if (event.event_path != path)
            return;
        apply_cursor_state(server);
    };
    if (auto directory = LexicalPath(path).dirname(); !directory.is_empty())
        (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
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
    bool check_config = false;
    bool print_config = false;
    Core::ArgsParser args_parser;
    args_parser.add_option(startup_command, "Command to run once the compositor is up", nullptr, 's', "command");
    args_parser.add_option(socket_name, "Wayland socket name", nullptr, 'n', "name");
    args_parser.add_option(check_config, "Validate the compositor config and exit", "check-config");
    args_parser.add_option(print_config, "Print the effective compositor config and exit", "print-config");
    args_parser.parse(arguments);

    if (check_config || print_config) {
        Config::load();
        if (print_config) {
            outln("[Workspaces]");
            outln("Rows={}", Config::workspaces().rows);
            outln("Columns={}", Config::workspaces().columns);
            outln("[Tiling]");
            outln("EdgeThreshold={}", Config::tiling().edge_threshold);
            outln("DragToEdge={}", Config::tiling().drag_to_edge);
            outln("[Focus]");
            outln("FollowsMouse={}", Config::focus().follows_mouse);
            outln("[Effects]");
            outln("WindowGeometry={}", Config::effects().window_geometry);
            outln("[Output]");
            outln("DefaultScale={}", Config::output().default_scale);
            outln("[Input]");
            outln("AccelMin={}", Config::input_ranges().accel_min);
            outln("AccelMax={}", Config::input_ranges().accel_max);
            outln("ScrollStepMin={}", Config::input_ranges().scroll_step_min);
            outln("DoubleClickMin={}", Config::input_ranges().double_click_min);
            outln("DoubleClickMax={}", Config::input_ranges().double_click_max);
            outln("[Fonts]");
            outln("Default={}", Config::fonts().default_query);
            outln("FixedWidth={}", Config::fonts().fixed_width_query);
            outln("WindowTitle={}", Config::fonts().window_title_query);
            outln("[Theme]");
            outln("Name={}", Config::theme().name);
            outln("Path={}", Config::theme().path);
            outln("[Applet]");
            StringBuilder order;
            for (size_t i = 0; i < Config::applet_order().size(); ++i) {
                if (i > 0)
                    order.append(',');
                order.append(Config::applet_order()[i]);
            }
            outln("Order={}", order.to_byte_string());
            outln("[Keybindings]");
            for (size_t i = 0; i < Config::action_names().size(); ++i)
                outln("{}={}", Config::action_names()[i], Config::binding_text(static_cast<Config::Action>(i)));
        }
        for (auto const& diagnostic : Config::diagnostics())
            warnln("sde-config: {}", diagnostic);
        if (!Config::diagnostics().is_empty())
            return 1;
        if (check_config)
            outln("sde-config: {} OK", Config::file_path());
        return 0;
    }

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
    WindowManager::set_host_hooks({ focus_toplevel, open_window_menu, primary_usable_area, toplevel_report_position, toplevel_begin_interactive, current_palette, update_ext_workspace_active });
    WindowSwitcher::set_host_hooks({ current_palette, focus_toplevel, WindowManager::switch_workspace });
    AppletManager::set_host_hooks({ WindowManager::tell_wms_applet_area_size_changed, primary_usable_area, layer_report_position });
    // Bring the workspace grid up from the config, and follow later edits.
    WindowManager::load_workspaces(*server);
    Config::watch([&] { WindowManager::load_workspaces(*server); });
    // R4.5: apply monitor-layout edits (screenlayout.ini) to the live outputs.
    Config::watch_outputs([&] { apply_loaded_screen_layout(*server); });
    // R4.6: apply input-setting edits (input.ini) to the live devices.
    Config::watch_input([&] { apply_input_settings(*server); });
    // R5: pick up app-requested global shortcuts (global-shortcuts.ini).
    Config::watch_global_shortcuts([&] { });
    apply_input_settings(*server);
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
    // R4.9: apply the persisted theme ([Theme]) at startup; the broker (a live
    // choice) then takes over.
    if (!Config::theme().path.is_empty())
        apply_theme_path(*server, Config::theme().path);
    watch_fonts_broker(*server);
    apply_cursor_state(*server);
    watch_cursor_state(*server);
    watch_wallpaper_state(*server);

    int result = loop.exec();
    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);
    return result;
}
