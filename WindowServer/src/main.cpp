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
#include "InputCapture.h"
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
#include <LibCore/ElapsedTimer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Path.h>
#include <LibGfx/Rect.h>
#include <LibGfx/StylePainter.h>
#include <LibGfx/SystemTheme.h>
#include <LibGfx/WindowTheme.h>
#include <LibIPC/Connection.h>
#include <LibMain/Main.h>
#include <WindowServer/WindowType.h>

#include <ctime>
#include <drm_fourcc.h>
#include <execinfo.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
#include <wlr/types/wlr_touch.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_switch.h>
#include <wlr/types/wlr_tablet_v2.h>
#include <wlr/types/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_pad.h>
#include <wlr/types/wlr_data_device.h>
// wlroots' xwayland.h declares a C field named `class`, which is a C++ keyword;
// rename it for the duration of the include (we never read that field).
#define class class_
#include <wlr/xwayland/xwayland.h>

// Read wlr_xwayland_surface::class (a C++ keyword) while the define is in scope.
static char const* xwayland_surface_class(wlr_xwayland_surface* surface) { return surface->class; }

#undef class
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_xdg_toplevel_icon_v1.h>
#include <wlr/types/wlr_xdg_system_bell_v1.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_ext_data_control_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_content_type_v1.h>
#include <wlr/types/wlr_tearing_control_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_xdg_dialog_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_keyboard_shortcuts_inhibit_v1.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
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
#include "serenity-virtual-input-server-protocol.h"
}

namespace {

// R6/N8: the compositor's verbosity ([Log] Level in WindowServer.ini, overridden
// by SDE_LOG_LEVEL). It sets wlroots' log level and gates our own messages.
enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };
static LogLevel g_log_level { LogLevel::Error };

static wlr_log_importance wlr_importance_for(LogLevel level)
{
    switch (level) {
    case LogLevel::Error:
    case LogLevel::Warn: // wlroots has no separate warn level
        return WLR_ERROR;
    case LogLevel::Info:
        return WLR_INFO;
    case LogLevel::Debug:
        return WLR_DEBUG;
    }
    return WLR_ERROR;
}

static Optional<LogLevel> log_level_from_string(StringView name)
{
    if (name == "error"sv)
        return LogLevel::Error;
    if (name == "warn"sv)
        return LogLevel::Warn;
    if (name == "info"sv)
        return LogLevel::Info;
    if (name == "debug"sv)
        return LogLevel::Debug;
    return {};
}

static bool should_log(LogLevel level) { return static_cast<int>(level) <= static_cast<int>(g_log_level); }

static void apply_log_level(LogLevel level, StringView name)
{
    g_log_level = level;
    wlr_log_init(wlr_importance_for(level), nullptr);
    if (should_log(LogLevel::Info))
        dbgln("WindowServer: log level '{}'", name);
}

// Tell the client where its window actually is (the compositor places it).
static void toplevel_report_position(Toplevel& toplevel);

// Idle notification (R4.1): reset the seat's idle timer on input.
static void notify_activity(Server& server);
// The client surface (xdg or X11) under a layout point, and focus for it (R5).
static wlr_surface* window_surface_at(Server& server, double lx, double ly, double* sx, double* sy);
static void focus_window_at(Server& server, double lx, double ly);
// X11 (Xwayland) window helpers (R5).
static X11Toplevel* x11_toplevel_for_wlr_surface(Server& server, wlr_surface* surface);
static bool x11_surface_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy);
static void focus_x11_toplevel(Server& server, X11Toplevel* toplevel);
static void x11_begin_interactive(X11Toplevel& toplevel, CursorMode mode, uint32_t edges);
// _NET_WM_ICON (fetch into a Gfx::Bitmap) and WM_CLASS (app identity); see
// docs/20-xwayland-next-steps.md item 3.
static void x11_fetch_icon(X11Toplevel& toplevel);
static void x11_update_class(X11Toplevel& toplevel);
static void x11_update_hints(X11Toplevel& toplevel);
static void x11_place_frame(X11Toplevel& toplevel);
// X11 server-side decoration frames (R5).
static void x11_update_frame(X11Toplevel& toplevel);
static bool handle_x11_frame_press(Server& server, uint32_t button);
static void handle_x11_frame_release(Server& server, uint32_t button);
static void update_x11_button_hover(Server& server, double x, double y);
static void use_nearest_filter(wlr_scene_node* node);
// While the session is locked, input goes to the lock surface on the output
// under the cursor.
static LockSurface* lock_surface_at_cursor(Server& server);
// Re-configure the lock surface for an output whose size/scale changed (R4.1).
static void reconfigure_lock_surface(Server& server, wlr_output* output);
// Spawn the configured lock client (R4.1); a standard ext-session-lock-v1 client.
static void spawn_lock_client();

static void toplevel_update_frame(Toplevel&);
static void toplevel_begin_interactive(Toplevel&, CursorMode, uint32_t edges);
static bool handle_frame_press(Server&, uint32_t button, uint32_t time);
static void handle_frame_release(Server&, uint32_t button);
static void close_window_menu(Server&);
static void update_window_menu_hover(Server&, double, double);
static void window_menu_handle_press(Server&, uint32_t button);
static void window_menu_handle_release(Server&, uint32_t button);
static bool window_menu_handle_key(Server&, xkb_keysym_t);
// Key repeat is compositor-owned (wlroots 0.20 removed its timer); see the
// repeat helpers below the keybinding code.
static void stop_key_repeat();
static void arrange_layers(Server&, wlr_output*);
static void update_xwayland_workareas(Server&);
static Output* output_for(Server&, wlr_output*);
static void output_manager_send_configuration(Server&);
static void persist_screen_layout(Server&);
static void schedule_persist_screen_layout(Server&);
static bool cycle_keyboard_layout(Server&);
static i32 g_next_wm_id { 1 };
static u64 g_focus_order_counter { 0 };
static Gfx::Palette* g_palette { nullptr };
// The active entry in [Keyboard] Layouts (Alt+Shift cycles it); the keymap is
// rebuilt with the list rotated by this offset. Reset when the config reloads.
static size_t s_layout_offset { 0 };

struct FrameIcons {
    RefPtr<Gfx::Bitmap> close;
    RefPtr<Gfx::Bitmap> minimize;
    RefPtr<Gfx::Bitmap> maximize;
    RefPtr<Gfx::Bitmap> restore;
    RefPtr<Gfx::Bitmap> move;
    RefPtr<Gfx::Bitmap> pin;
};
static FrameIcons g_frame_icons;

// The default window icon (SerenityOS WindowServer's default: window.png), used
// for toplevels whose client has not supplied one, so every window has an icon
// (titlebar, switcher, Taskbar) and the Taskbar never receives an invalid icon.
static RefPtr<Gfx::Bitmap> g_default_window_icon;

// A distinct default icon for **X11 (Xwayland)** clients that supply none: a big
// monochrome "X" (in the spirit of the X11 logo), so an X11 window's titlebar is
// recognisable at a glance (vs. the generic window icon). Drawn in the titlebar's
// text colour at 64x64 and downscaled to 16x16 (the titlebar/switcher size) for
// crisp, anti-aliased diagonals.
static RefPtr<Gfx::Bitmap> g_x11_default_icon;

static RefPtr<Gfx::Bitmap> make_x11_default_icon(Gfx::Color ink)
{
    constexpr int big = 64;
    constexpr int small = 16;

    auto canvas = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { big, big });
    if (canvas.is_error())
        return nullptr;
    auto source = canvas.release_value();
    Gfx::Painter painter(*source);
    painter.clear_rect(source->rect(), Gfx::Color::Transparent);

    // A big "X" (no window frame), taller than it is wide like the X11 logo.
    Gfx::Path cross;
    cross.move_to({ 19, 5 });
    cross.line_to({ 45, 59 });
    cross.move_to({ 45, 5 });
    cross.line_to({ 19, 59 });
    painter.stroke_path(cross, ink, 10);

    auto result = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { small, small });
    if (result.is_error())
        return nullptr;
    auto downscaled = result.release_value();
    Gfx::Painter downscaled_painter(*downscaled);
    downscaled_painter.clear_rect(downscaled->rect(), Gfx::Color::Transparent);
    downscaled_painter.draw_scaled_bitmap(downscaled->rect(), *source, source->rect(), 1.0f, Gfx::ScalingMode::BilinearBlend);
    return downscaled;
}

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

// The theme's overlay rect shadow (a 9-slice bitmap) behind the window switcher,
// like SerenityOS WindowManager::overlay_rect_shadow().
static ByteString g_resource_root;
static RefPtr<Gfx::Bitmap> g_overlay_shadow;

static void load_overlay_shadow()
{
    g_overlay_shadow = nullptr;
    if (!g_palette)
        return;
    auto path = g_palette->overlay_rect_shadow_path();
    if (path.is_empty())
        return;
    ByteString resolved = path;
    if (path.starts_with("/res"sv))
        resolved = ByteString::formatted("{}{}", g_resource_root, path.substring_view(4));
    if (auto bitmap = Gfx::Bitmap::load_from_file(resolved); !bitmap.is_error()) {
        g_overlay_shadow = bitmap.release_value();
        dbgln("WindowServer: loaded overlay rect shadow '{}' ({}x{})", resolved, g_overlay_shadow->width(), g_overlay_shadow->height());
    } else {
        warnln("WindowServer: could not load the overlay rect shadow '{}': {}", resolved, bitmap.error());
    }
}

static Gfx::Bitmap const* overlay_shadow_hook()
{
    return g_overlay_shadow.ptr();
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

    // A toplevel taking focus ends any on_demand layer surface's keyboard focus.
    server.keyboard_focus_layer = nullptr;

    // A focus change retargets the keyboard; stop repeating into the old client.
    stop_key_repeat();

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

// --- key repeat -----------------------------------------------------------
//
// wlroots 0.20 no longer runs a key-repeat timer for the compositor:
// wlr_keyboard_set_repeat_info() only advertises the rate/delay to clients
// (wl_keyboard.repeat_info) and does not synthesise repeats. So the compositor
// owns repeat: after [Keyboard] RepeatDelay it re-sends the held key to the
// focused surface, then again every 1000/RepeatRate ms, until the key is
// released. A fresh forwarded press becomes the repeating key; keys the
// compositor consumes as keybindings, and keys the keymap marks non-repeating
// (modifiers), never repeat.
static RefPtr<Core::Timer> s_key_repeat_timer;
static Keyboard* s_key_repeat_keyboard { nullptr };
static uint32_t s_key_repeat_keycode { 0 };

static u32 monotonic_time_msec()
{
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<u32>(static_cast<u64>(now.tv_sec) * 1000 + static_cast<u64>(now.tv_nsec) / 1000000);
}

static void stop_key_repeat()
{
    if (s_key_repeat_timer)
        s_key_repeat_timer->stop();
    s_key_repeat_keyboard = nullptr;
}

// A single-shot timer: it first fires after the delay, then re-arms itself at
// the steady repeat rate.
static void key_repeat_timer_fired()
{
    if (!s_key_repeat_keyboard) {
        stop_key_repeat();
        return;
    }
    auto& server = *s_key_repeat_keyboard->server;
    // A held key is input activity: keep the seat from going idle mid-key.
    notify_activity(server);
    wlr_seat_set_keyboard(server.seat, s_key_repeat_keyboard->keyboard);
    wlr_seat_keyboard_notify_key(server.seat, monotonic_time_msec(), s_key_repeat_keycode, WL_KEYBOARD_KEY_STATE_PRESSED);
    // Repeat is generated outside the Wayland event dispatch, so flush the
    // queued key event to the client now instead of waiting for the next
    // unrelated Wayland event (which would deliver repeats in bursts).
    wl_display_flush_clients(server.display);

    auto rate = Config::input().keyboard.repeat_rate;
    if (rate <= 0) {
        stop_key_repeat();
        return;
    }
    s_key_repeat_timer->start(max(1000 / rate, 1));
}

static void start_key_repeat(Keyboard& keyboard, uint32_t keycode)
{
    auto const& settings = Config::input().keyboard;
    auto* keymap = keyboard.keyboard->keymap;
    if (settings.repeat_rate <= 0 || !keymap || !xkb_keymap_key_repeats(keymap, keycode + 8)) {
        stop_key_repeat();
        return;
    }
    if (!s_key_repeat_timer)
        s_key_repeat_timer = Core::Timer::create_single_shot(max(settings.repeat_delay, 1), key_repeat_timer_fired);
    s_key_repeat_keyboard = &keyboard;
    s_key_repeat_keycode = keycode;
    s_key_repeat_timer->restart(max(settings.repeat_delay, 1));
}

// A client (a game, a VM viewer) can inhibit the compositor's keybindings for its
// focused surface (zwp_keyboard_shortcuts_inhibit_v1). While active, keys are
// forwarded to the client without triggering compositor actions.
static bool keyboard_shortcuts_inhibited(Server& server, wlr_surface* surface)
{
    if (!server.keyboard_shortcuts_inhibit || !surface)
        return false;
    wlr_keyboard_shortcuts_inhibitor_v1* inhibitor = nullptr;
    wl_list_for_each(inhibitor, &server.keyboard_shortcuts_inhibit->inhibitors, link) {
        if (inhibitor->active && inhibitor->surface == surface)
            return true;
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

    // Releasing the held key ends its repeat; unrelated releases are ignored.
    if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && s_key_repeat_keyboard && event->keycode == s_key_repeat_keycode)
        stop_key_repeat();

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

    // R5: InputCapture — while keyboard capture is active, keys go to the EIS client.
    if (InputCapture::handle_keyboard_key(server, event->keycode, event->state == WL_KEYBOARD_KEY_STATE_PRESSED,
            keyboard->keyboard->modifiers.depressed, keyboard->keyboard->modifiers.latched,
            keyboard->keyboard->modifiers.locked, keyboard->keyboard->modifiers.group))
        return;

    // While the session is locked, keys go to the lock surface, not the shell.
    if (server.locked) {
        if (auto* lock = lock_surface_at_cursor(server)) {
            wlr_seat_set_keyboard(server.seat, keyboard->keyboard);
            wlr_seat_keyboard_notify_enter(server.seat, lock->lock_surface->surface, keyboard->keyboard->keycodes, keyboard->keyboard->num_keycodes, &keyboard->keyboard->modifiers);
            wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
            if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
                start_key_repeat(*keyboard, event->keycode);
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
            stop_key_repeat();
            for (int i = 0; i < syms_count; ++i)
                window_menu_handle_key(server, syms[i]);
        }
        return;
    }
    bool inhibited = keyboard_shortcuts_inhibited(server, server.seat->keyboard_state.focused_surface);
    if (!inhibited && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // R4.4: configured keybindings (defaults match the historical bindings).
        u32 const config_mods = config_modifiers(modifiers);
        // SerenityOS: Alt+Shift switches the keyboard layout (KeymapSwitcher).
        if (config_mods == (Config::ModAlt | Config::ModShift)) {
            for (int i = 0; i < syms_count; ++i) {
                if (syms[i] == XKB_KEY_Shift_L || syms[i] == XKB_KEY_Shift_R || syms[i] == XKB_KEY_Alt_L || syms[i] == XKB_KEY_Alt_R) {
                    handled |= cycle_keyboard_layout(server);
                    break;
                }
            }
        }
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
    if (!inhibited) {
        // The window switcher (Alt+Tab / Super+Tab) grabs the keyboard while open.
        bool switcher_handled = false;
        for (int i = 0; i < syms_count; ++i)
            switcher_handled |= WindowSwitcher::handle_key(server, syms[i], modifiers, event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
        if (switcher_handled) {
            // A Super+Tab/Super+Shift+Tab shortcut is not a Super tap.
            WindowManager::clear_super_tap();
            return;
        }
        for (int i = 0; i < syms_count; ++i)
            handled |= WindowManager::handle_super_key(server, syms[i], modifiers, event->state == WL_KEYBOARD_KEY_STATE_PRESSED);
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            for (int i = 0; i < syms_count; ++i)
                handled |= handle_keybinding(server, syms[i], modifiers);
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(server.seat, keyboard->keyboard);
        wlr_seat_keyboard_notify_key(server.seat, event->time_msec, event->keycode, event->state);
        if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
            start_key_repeat(*keyboard, event->keycode);
    } else if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        // A key the compositor consumed (a keybinding) does not repeat.
        stop_key_repeat();
    }
}

// Advertise the seat's current capabilities to clients. Kept in one place so a
// hotplug adds the keyboard capability and an unplug removes it again; otherwise
// the seat keeps advertising a keyboard that is no longer there.
static void update_seat_capabilities(Server& server)
{
    uint32_t capabilities = WL_SEAT_CAPABILITY_POINTER;
    if (!server.keyboards.is_empty())
        capabilities |= WL_SEAT_CAPABILITY_KEYBOARD;
    if (!server.touch_devices.is_empty() || server.virtual_touch_count > 0)
        capabilities |= WL_SEAT_CAPABILITY_TOUCH;
    wlr_seat_set_capabilities(server.seat, capabilities);
}

static void keyboard_destroy(wl_listener* listener, void*)
{
    Keyboard* keyboard = nullptr;
    keyboard = wl_container_of(listener, keyboard, destroy);
    auto& server = *keyboard->server;
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->destroy.link);
    if (s_key_repeat_keyboard == keyboard)
        stop_key_repeat();
    remove_owned(server.keyboards, keyboard);
    // Drop the keyboard capability once the last keyboard is gone, so clients do
    // not keep a stale wl_keyboard (a later client would otherwise get one with
    // no keymap). Symmetric with server_new_input().
    update_seat_capabilities(server);
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
    server.grabbed_x11 = nullptr;
    // The interactive grab ended: hide the geometry HUD (WS2).
    WindowManager::hide_geometry_hud();
}

static void process_cursor_move(Server& server)
{
    if (server.grabbed_x11) {
        auto* toplevel = server.grabbed_x11;
        int x = static_cast<int>(server.cursor->x - server.grab_x);
        int y = static_cast<int>(server.cursor->y - server.grab_y);
        wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
        wlr_xwayland_surface_configure(toplevel->xsurface, x, y, toplevel->xsurface->width, toplevel->xsurface->height);
        return;
    }
    auto* toplevel = server.grabbed_toplevel;
    if (!toplevel)
        return;
    wlr_scene_node_set_position(&toplevel->scene_tree->node, server.cursor->x - server.grab_x, server.cursor->y - server.grab_y);
    // Show the tile preview when the pointer reaches a screen edge (WS2).
    WindowManager::update_move_snap(server, *toplevel);
}

static void process_cursor_resize(Server& server)
{
    if (server.grabbed_x11) {
        auto* toplevel = server.grabbed_x11;
        double border_x = server.cursor->x - server.grab_x;
        double border_y = server.cursor->y - server.grab_y;
        int new_left = server.grab_box.x;
        int new_right = server.grab_box.x + server.grab_box.width;
        int new_top = server.grab_box.y;
        int new_bottom = server.grab_box.y + server.grab_box.height;
        if (server.resize_edges & WLR_EDGE_TOP) {
            new_top = static_cast<int>(border_y);
            if (new_top >= new_bottom) new_top = new_bottom - 1;
        } else if (server.resize_edges & WLR_EDGE_BOTTOM) {
            new_bottom = static_cast<int>(border_y);
            if (new_bottom <= new_top) new_bottom = new_top + 1;
        }
        if (server.resize_edges & WLR_EDGE_LEFT) {
            new_left = static_cast<int>(border_x);
            if (new_left >= new_right) new_left = new_right - 1;
        } else if (server.resize_edges & WLR_EDGE_RIGHT) {
            new_right = static_cast<int>(border_x);
            if (new_right <= new_left) new_right = new_left + 1;
        }
        // new_left/top/right/bottom are frame edges; the client's size is the
        // content (frame minus the decorations).
        int const frame_left = toplevel->frame_decorated ? toplevel->frame_left : 0;
        int const frame_top = toplevel->frame_decorated ? toplevel->frame_top : 0;
        int const frame_right = toplevel->frame_decorated ? toplevel->frame_right : 0;
        int const frame_bottom = toplevel->frame_decorated ? toplevel->frame_bottom : 0;
        int content_w = new_right - new_left - frame_left - frame_right;
        int content_h = new_bottom - new_top - frame_top - frame_bottom;
        if (content_w < 1) content_w = 1;
        if (content_h < 1) content_h = 1;
        wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left, new_top);
        wlr_xwayland_surface_configure(toplevel->xsurface, new_left, new_top, content_w, content_h);
        return;
    }
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
    X11Toplevel* x11_toplevel { nullptr };
    FrameRegion region { FrameRegion::None };
    int button_index { -1 };
    uint32_t edges { 0 };
    Gfx::IntPoint local;
};

static FrameHit x11_hit_test_frame(Server& server, double x, double y);

static FrameHit hit_test_frame(Server& server, double x, double y)
{
    FrameHit hit;
    for (size_t index = server.toplevels.size(); index-- > 0;) {
        auto& toplevel = *server.toplevels[index];
        if (toplevel.minimized)
            continue;
        // A client-side-decorated window has no compositor frame, so it has no
        // frame regions to hit-test; the client handles move/resize itself.
        if (toplevel.client_side_decorations)
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

// zwp-pointer-constraints-v1 (R5). wlroots advertises the protocol; the
// compositor owns enforcement. It activates the constraint of the surface under
// the pointer and, while active, keeps the cursor locked to the client's hint or
// confined to its region.
struct PointerConstraintListener {
    wl_listener destroy;
    Server* server { nullptr };
};

static void pointer_constraint_destroy(wl_listener* listener, void* data)
{
    auto* constraint = static_cast<wlr_pointer_constraint_v1*>(data);
    PointerConstraintListener* entry = nullptr;
    entry = wl_container_of(listener, entry, destroy);
    if (entry->server->active_pointer_constraint == constraint)
        entry->server->active_pointer_constraint = nullptr;
    wl_list_remove(&listener->link);
    delete entry;
}

static void new_pointer_constraint(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_pointer_constraint);
    auto* constraint = static_cast<wlr_pointer_constraint_v1*>(data);
    auto* entry = new PointerConstraintListener { {}, server };
    entry->destroy.notify = pointer_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &entry->destroy);
}

// The layout-space origin of a surface, for a constraint on an xdg toplevel (the
// case games use). Returns false for other surfaces.
static bool surface_global_origin(Server& server, wlr_surface* surface, double* out_x, double* out_y)
{
    for (auto& toplevel : server.toplevels) {
        if (!toplevel->xdg_toplevel || toplevel->xdg_toplevel->base->surface != surface)
            continue;
        int lx = 0;
        int ly = 0;
        if (!wlr_scene_node_coords(&toplevel->content_tree->node, &lx, &ly))
            return false;
        *out_x = lx;
        *out_y = ly;
        return true;
    }
    return false;
}

// Explicit drag-and-drop (data-device DnD between windows). A client asks the
// seat to start a drag; we validate and accept it, wlroots installs the pointer
// grab that turns our normal pointer notifications into wl_data_device
// enter/motion/drop, and we draw the drag icon above everything, following the
// cursor.
static void destroy_drag_icon_node(Server& server)
{
    if (server.drag_icon_node) {
        wlr_scene_node_destroy(server.drag_icon_node);
        server.drag_icon_node = nullptr;
    }
}

static void drag_icon_destroyed(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag_icon_destroy);
    destroy_drag_icon_node(*server);
    wl_list_remove(&listener->link);
}

static void drag_destroyed(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, drag_destroy);
    destroy_drag_icon_node(*server);
    server->active_drag = nullptr;
    server->drag_target = nullptr;
    // wlroots asserts that no listeners remain on the signal after it is emitted.
    wl_list_remove(&listener->link);
    dbgln("WindowServer: drag ended");
}

static void seat_start_drag(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_start_drag);
    auto* drag = static_cast<wlr_drag*>(data);
    server->active_drag = drag;
    server->drag_target = nullptr;
    server->drag_destroy.notify = drag_destroyed;
    wl_signal_add(&drag->events.destroy, &server->drag_destroy);
    if (drag->icon && drag->icon->surface && server->drag_icon_tree) {
        auto* scene = wlr_scene_surface_create(server->drag_icon_tree, drag->icon->surface);
        server->drag_icon_node = &scene->buffer->node;
        wlr_scene_node_set_position(server->drag_icon_node, static_cast<int>(server->cursor->x), static_cast<int>(server->cursor->y));
        server->drag_icon_destroy.notify = drag_icon_destroyed;
        wl_signal_add(&drag->icon->events.destroy, &server->drag_icon_destroy);
    }
    dbgln("WindowServer: drag started (icon={})", server->drag_icon_node ? "yes" : "no");
}

static void seat_request_start_drag(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_request_start_drag);
    auto* event = static_cast<wlr_seat_request_start_drag_event*>(data);
    // The client uses the same start_drag request for pointer and touch, so the
    // serial (a recent pointer grab or touch point) decides which grab to start;
    // and it proves the client may start a drag at all.
    wlr_touch_point* point = nullptr;
    if (wlr_seat_validate_touch_grab_serial(server->seat, event->origin, event->serial, &point)) {
        wlr_seat_start_touch_drag(server->seat, event->drag, event->serial, point);
    } else if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
        wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
    } else if (event->drag->source) {
        wlr_data_source_destroy(event->drag->source);
    }
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

    // Explicit drag-and-drop: route motion to the surface under the cursor (the
    // drag grab turns it into wl_data_device enter/motion) and follow the cursor
    // with the drag icon.
    if (server.active_drag) {
        double dx = 0;
        double dy = 0;
        wlr_surface* target = nullptr;
        toplevel_at(server, server.cursor->x, server.cursor->y, &target, &dx, &dy);
        if (target != server.drag_target) {
            server.drag_target = target;
            if (target)
                wlr_seat_pointer_notify_enter(server.seat, target, dx, dy);
            else
                wlr_seat_pointer_clear_focus(server.seat);
        } else if (target) {
            wlr_seat_pointer_notify_motion(server.seat, time, dx, dy);
        }
        if (server.drag_icon_node)
            wlr_scene_node_set_position(server.drag_icon_node, static_cast<int>(server.cursor->x), static_cast<int>(server.cursor->y));
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
    update_x11_button_hover(server, server.cursor->x, server.cursor->y);

    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    if (hit.region == FrameRegion::None)
        surface = window_surface_at(server, server.cursor->x, server.cursor->y, &sx, &sy);

    // Pointer constraints: activate the constraint of the surface under the
    // cursor and, while one is active, keep the cursor locked or confined.
    if (server.pointer_constraints) {
        wlr_pointer_constraint_v1* constraint = surface
            ? wlr_pointer_constraints_v1_constraint_for_surface(server.pointer_constraints, surface, server.seat)
            : nullptr;
        if (constraint != server.active_pointer_constraint) {
            if (server.active_pointer_constraint)
                wlr_pointer_constraint_v1_send_deactivated(server.active_pointer_constraint);
            server.active_pointer_constraint = constraint;
            if (constraint)
                wlr_pointer_constraint_v1_send_activated(constraint);
        }
        if (auto* active = server.active_pointer_constraint) {
            double origin_x = 0;
            double origin_y = 0;
            if (surface_global_origin(server, active->surface, &origin_x, &origin_y)) {
                if (active->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
                    if (active->current.cursor_hint.enabled)
                        wlr_cursor_warp_closest(server.cursor, nullptr, origin_x + active->current.cursor_hint.x, origin_y + active->current.cursor_hint.y);
                } else if (active->type == WLR_POINTER_CONSTRAINT_V1_CONFINED && (active->current.committed & WLR_POINTER_CONSTRAINT_V1_STATE_REGION)) {
                    double const local_x = server.cursor->x - origin_x;
                    double const local_y = server.cursor->y - origin_y;
                    if (!pixman_region32_contains_point(&active->current.region, local_x, local_y, nullptr)) {
                        auto const* box = pixman_region32_extents(&active->current.region);
                        double const clamped_x = min(max(local_x, static_cast<double>(box->x1)), static_cast<double>(box->x2));
                        double const clamped_y = min(max(local_y, static_cast<double>(box->y1)), static_cast<double>(box->y2));
                        wlr_cursor_warp_closest(server.cursor, nullptr, origin_x + clamped_x, origin_y + clamped_y);
                    }
                }
                // Recompute surface-local coordinates for the (possibly warped) cursor.
                sx = server.cursor->x - origin_x;
                sy = server.cursor->y - origin_y;
            }
        }
    }

    if (surface) {
        if (server.seat->pointer_state.focused_surface != surface)
            wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, "default");
        wlr_seat_pointer_notify_enter(server.seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server.seat, time, sx, sy);
    } else {
        wlr_seat_pointer_clear_focus(server.seat);
    }

    // [Focus] FollowsMouse (R4.4): focus the window (xdg or X11) under the cursor.
    if (Config::focus().follows_mouse && server.cursor_mode == CursorMode::Passthrough && !server.pointer_grab_surface)
        focus_window_at(server, server.cursor->x, server.cursor->y);

    // Set the cursor for our chrome last: clearing client focus above resets it
    // to the default arrow, so the resize cursor must come after.
    if (hit.region != FrameRegion::None) {
        wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, hit.region == FrameRegion::Border ? resize_cursor_name(hit.edges) : "default");
    } else {
        auto x11_hit = x11_hit_test_frame(server, server.cursor->x, server.cursor->y);
        if (x11_hit.region == FrameRegion::Border)
            wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, resize_cursor_name(x11_hit.edges));
    }
}

static void cursor_motion(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, cursor_motion);
    auto* event = static_cast<wlr_pointer_motion_event*>(data);
    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);
    // R5: relative-pointer clients (games) get the raw relative motion. The time
    // is in microseconds (wl_pointer uses milliseconds).
    if (server->relative_pointer_manager)
        wlr_relative_pointer_manager_v1_send_relative_motion(server->relative_pointer_manager, server->seat,
            static_cast<uint64_t>(event->time_msec) * 1000, event->delta_x, event->delta_y, event->unaccel_dx, event->unaccel_dy);
    move_cursor_highlight(*server);
    // The switcher grabs the pointer while it is open.
    if (WindowSwitcher::handle_pointer_motion(*server, server->cursor->x, server->cursor->y)) {
        notify_activity(*server);
        return;
    }
    if (!InputCapture::handle_pointer_motion(*server, event->time_msec))
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
    // The switcher grabs the pointer while it is open.
    if (WindowSwitcher::handle_pointer_motion(*server, server->cursor->x, server->cursor->y)) {
        notify_activity(*server);
        return;
    }
    if (!InputCapture::handle_pointer_motion(*server, event->time_msec))
        process_cursor_motion(*server, event->time_msec);
    notify_activity(*server);
}

// Touch routing, shared by real touch devices (via the cursor) and the harness's
// virtual touch (serenity-virtual-input). Coordinates are in the output layout.
static void handle_touch_down(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly)
{
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    if (auto* toplevel = toplevel_at(server, lx, ly, &surface, &sx, &sy))
        focus_toplevel(server, toplevel);
    if (surface)
        wlr_seat_touch_notify_down(server.seat, surface, time_msec, touch_id, sx, sy);
    notify_activity(server);
}

static void handle_touch_motion(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly)
{
    // A touch drag's icon follows the finger (the pointer cursor does not move).
    if (server.active_drag && server.drag_icon_node)
        wlr_scene_node_set_position(server.drag_icon_node, static_cast<int>(lx), static_cast<int>(ly));
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    toplevel_at(server, lx, ly, &surface, &sx, &sy);
    if (surface)
        wlr_seat_touch_notify_motion(server.seat, time_msec, touch_id, sx, sy);
    notify_activity(server);
}

static void handle_touch_up(Server& server, uint32_t time_msec, int32_t touch_id)
{
    wlr_seat_touch_notify_up(server.seat, time_msec, touch_id);
    notify_activity(server);
}

static void handle_touch_cancel(Server& server)
{
    // A device-level cancel: send wl_touch.cancel to every client with touch points.
    wlr_seat_client* seat_client = nullptr;
    wl_list_for_each(seat_client, &server.seat->clients, link) {
        if (!wl_list_empty(&seat_client->touches))
            wlr_seat_touch_notify_cancel(server.seat, seat_client);
    }
}

static void cursor_touch_down(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, touch_down);
    auto* event = static_cast<wlr_touch_down_event*>(data);
    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y, &lx, &ly);
    handle_touch_down(*server, event->time_msec, event->touch_id, lx, ly);
}

static void cursor_touch_motion(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, touch_motion);
    auto* event = static_cast<wlr_touch_motion_event*>(data);
    double lx = 0;
    double ly = 0;
    wlr_cursor_absolute_to_layout_coords(server->cursor, &event->touch->base, event->x, event->y, &lx, &ly);
    handle_touch_motion(*server, event->time_msec, event->touch_id, lx, ly);
}

static void cursor_touch_up(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, touch_up);
    auto* event = static_cast<wlr_touch_up_event*>(data);
    handle_touch_up(*server, event->time_msec, event->touch_id);
}

static void cursor_touch_cancel(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, touch_cancel);
    (void)data;
    handle_touch_cancel(*server);
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

// SerenityOS lets you grab a window anywhere to move it (Super+left-drag) or
// resize it (Super+right-drag), without reaching for the titlebar or an edge;
// the resize direction is the window's 3x3 hot area under the cursor.
static bool handle_super_drag_press(Server& server, uint32_t button)
{
    if (button != BTN_LEFT && button != BTN_RIGHT)
        return false;
    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (!keyboard || !(wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO))
        return false;

    wlr_surface* surface = nullptr;
    double sx = 0;
    double sy = 0;
    auto* toplevel = toplevel_at(server, server.cursor->x, server.cursor->y, &surface, &sx, &sy);
    if (!toplevel) {
        // X11 windows get the same Super+<button> move/resize (KWin's Alt+drag).
        if (!x11_surface_at(server, server.cursor->x, server.cursor->y, &surface, &sx, &sy))
            return false;
        auto* x11 = x11_toplevel_for_wlr_surface(server, surface);
        if (!x11 || x11->override_redirect)
            return false;
        focus_x11_toplevel(server, x11);
        if (button == BTN_LEFT) {
            x11_begin_interactive(*x11, CursorMode::Move, 0);
            return true;
        }
        int width = x11->xsurface->width > 0 ? x11->xsurface->width : 1;
        int height = x11->xsurface->height > 0 ? x11->xsurface->height : 1;
        double relative_x = (server.cursor->x - x11->scene_tree->node.x) / width;
        double relative_y = (server.cursor->y - x11->scene_tree->node.y) / height;
        uint32_t edges = 0;
        if (relative_x < 1.0 / 3.0)
            edges |= WLR_EDGE_LEFT;
        else if (relative_x > 2.0 / 3.0)
            edges |= WLR_EDGE_RIGHT;
        if (relative_y < 1.0 / 3.0)
            edges |= WLR_EDGE_TOP;
        else if (relative_y > 2.0 / 3.0)
            edges |= WLR_EDGE_BOTTOM;
        if (edges == 0)
            return false;
        x11_begin_interactive(*x11, CursorMode::Resize, edges);
        return true;
    }
    if (toplevel->fullscreen)
        return false;

    if (button == BTN_LEFT) {
        focus_toplevel(server, toplevel);
        toplevel_begin_interactive(*toplevel, CursorMode::Move, 0);
        return true;
    }

    int width = toplevel->frame_size.width() > 0 ? toplevel->frame_size.width() : toplevel->xdg_toplevel->base->geometry.width;
    int height = toplevel->frame_size.height() > 0 ? toplevel->frame_size.height() : toplevel->xdg_toplevel->base->geometry.height;
    if (width <= 0 || height <= 0)
        return false;
    auto* node = &toplevel->scene_tree->node;
    double relative_x = (server.cursor->x - node->x) / width;
    double relative_y = (server.cursor->y - node->y) / height;
    uint32_t edges = 0;
    if (relative_x < 1.0 / 3.0)
        edges |= WLR_EDGE_LEFT;
    else if (relative_x > 2.0 / 3.0)
        edges |= WLR_EDGE_RIGHT;
    if (relative_y < 1.0 / 3.0)
        edges |= WLR_EDGE_TOP;
    else if (relative_y > 2.0 / 3.0)
        edges |= WLR_EDGE_BOTTOM;
    if (edges == 0)
        return false;

    focus_toplevel(server, toplevel);
    toplevel_begin_interactive(*toplevel, CursorMode::Resize, edges);
    return true;
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

    // The Alt+Tab switcher grabs the pointer while it is open.
    if (WindowSwitcher::handle_pointer_button(*server, event->button, event->state))
        return;

    // R5: InputCapture — while a capture is active, buttons go to the EIS client.
    if (InputCapture::handle_pointer_button(*server, event->button, event->state, event->time_msec))
        return;

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
        // Using Super with the mouse (move/resize, or a click) is not a Super
        // tap, so do not open the Taskbar menu when Super is released.
        if (auto* keyboard = wlr_seat_get_keyboard(server->seat); keyboard && (wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO))
            WindowManager::clear_super_tap();
        if (handle_frame_press(*server, event->button, event->time_msec)) {
            // Opening the window menu is not an interactive frame grab.
            if (!server->window_menu.open)
                server->frame_grab = true;
            return;
        }
        if (handle_x11_frame_press(*server, event->button)) {
            server->frame_grab = true;
            return;
        }
        // SerenityOS: Super+LMB moves and Super+RMB resizes, from anywhere.
        if (handle_super_drag_press(*server, event->button)) {
            server->frame_grab = true;
            return;
        }
    } else if (server->frame_grab) {
        server->frame_grab = false;
        // A titlebar drag has ended: tile to the previewed edge target, if any.
        if (server->grabbed_toplevel && server->cursor_mode == CursorMode::Move)
            WindowManager::finish_move_snap(*server, *server->grabbed_toplevel);
        handle_frame_release(*server, event->button);
        handle_x11_frame_release(*server, event->button);
        if (server->grabbed_x11)
            reset_cursor_mode(*server);
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
        wlr_surface* surface = window_surface_at(*server, server->cursor->x, server->cursor->y, &sx, &sy);
        focus_window_at(*server, server->cursor->x, server->cursor->y);
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

// PRIMARY selection (zwp_primary_selection_v1 / gtk): forward it to the seat so
// wlroots' XWM can bridge it to X11 PRIMARY and vice-versa (docs/20 item 5).
static void seat_request_set_primary_selection(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, seat_request_set_primary_selection);
    auto* event = static_cast<wlr_seat_request_set_primary_selection_event*>(data);
    wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

// --- input device configuration (R4.6) ------------------------------------

// The configured layouts rotated so `offset` is first, so Alt+Shift can switch
// the active layout by rebuilding the keymap (xkb's group order follows the
// layout list). A single layout is returned unchanged.
static ByteString rotated_layouts(ByteString const& layouts, size_t offset)
{
    auto parts = layouts.split(',');
    if (parts.size() < 2 || offset == 0)
        return layouts;
    offset %= parts.size();
    StringBuilder builder;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            builder.append(',');
        builder.append(parts[(i + offset) % parts.size()].trim_whitespace());
    }
    return builder.to_byte_string();
}

// Build the xkb keymap from the configured layouts/variant/model/options and
// apply it (with repeat info) to every keyboard, falling back to the default
// keymap when the configuration does not compile.
static void apply_keyboard_input(Server& server)
{
    auto const& settings = Config::input().keyboard;
    auto model = settings.model;
    auto layout = rotated_layouts(settings.layouts, s_layout_offset);
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
    // Repeat is compositor-owned (wlroots 0.20 removed the timer); drop any
    // in-flight repeat if the rate was turned off.
    if (settings.repeat_rate <= 0)
        stop_key_repeat();
}

// Alt+Shift: advance to the next configured keyboard layout (SerenityOS's
// KeymapSwitcher). Rebuilds the keymap with the layout list rotated; a no-op with
// a single layout (so the chord still reaches the focused client).
static bool cycle_keyboard_layout(Server& server)
{
    auto layouts = Config::input().keyboard.layouts.split(',');
    if (layouts.size() < 2)
        return false;
    s_layout_offset = (s_layout_offset + 1) % layouts.size();
    apply_keyboard_input(server);
    dbgln("WindowServer: keyboard layout -> '{}'", layouts[s_layout_offset].trim_whitespace());
    return true;
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
    s_layout_offset = 0;
    apply_keyboard_input(server);
    apply_mouse_input(server);
    dbgln("WindowServer: applied input settings (accel {}, natural-scroll {}, buttons-switched {}, left-handed {}, layouts '{}')",
        Config::input().mouse.acceleration, Config::input().mouse.natural_scroll ? "on" : "off",
        Config::input().mouse.buttons_switched ? "on" : "off", Config::input().mouse.left_handed ? "on" : "off", Config::input().keyboard.layouts);
}

// wp-pointer-gestures-v1: forward touchpad swipe/pinch/hold to clients. libinput
// reports them per pointer device; the compositor relays them on the seat.
static void pointer_swipe_begin(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, swipe_begin);
    auto* event = static_cast<wlr_pointer_swipe_begin_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_begin(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->fingers);
    notify_activity(*pointer->server);
}

static void pointer_swipe_update(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, swipe_update);
    auto* event = static_cast<wlr_pointer_swipe_update_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_update(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->dx, event->dy);
    notify_activity(*pointer->server);
}

static void pointer_swipe_end(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, swipe_end);
    auto* event = static_cast<wlr_pointer_swipe_end_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_swipe_end(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->cancelled);
    notify_activity(*pointer->server);
}

static void pointer_pinch_begin(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, pinch_begin);
    auto* event = static_cast<wlr_pointer_pinch_begin_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_begin(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->fingers);
    notify_activity(*pointer->server);
}

static void pointer_pinch_update(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, pinch_update);
    auto* event = static_cast<wlr_pointer_pinch_update_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_update(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->dx, event->dy, event->scale, event->rotation);
    notify_activity(*pointer->server);
}

static void pointer_pinch_end(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, pinch_end);
    auto* event = static_cast<wlr_pointer_pinch_end_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_pinch_end(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->cancelled);
    notify_activity(*pointer->server);
}

static void pointer_hold_begin(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, hold_begin);
    auto* event = static_cast<wlr_pointer_hold_begin_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_hold_begin(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->fingers);
    notify_activity(*pointer->server);
}

static void pointer_hold_end(wl_listener* listener, void* data)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, hold_end);
    auto* event = static_cast<wlr_pointer_hold_end_event*>(data);
    if (pointer->server->pointer_gestures)
        wlr_pointer_gestures_v1_send_hold_end(pointer->server->pointer_gestures, pointer->server->seat, event->time_msec, event->cancelled);
    notify_activity(*pointer->server);
}

static void pointer_destroy(wl_listener* listener, void*)
{
    Pointer* pointer = nullptr;
    pointer = wl_container_of(listener, pointer, destroy);
    wl_list_remove(&pointer->swipe_begin.link);
    wl_list_remove(&pointer->swipe_update.link);
    wl_list_remove(&pointer->swipe_end.link);
    wl_list_remove(&pointer->pinch_begin.link);
    wl_list_remove(&pointer->pinch_update.link);
    wl_list_remove(&pointer->pinch_end.link);
    wl_list_remove(&pointer->hold_begin.link);
    wl_list_remove(&pointer->hold_end.link);
    wl_list_remove(&pointer->destroy.link);
    remove_owned(pointer->server->pointer_objects, pointer);
}

// Switch devices: lid, tablet-mode, keypad-slide. The compositor forwards the
// state (a lid close is a natural place to idle/lock) rather than owning policy.
static void switch_toggle(wl_listener* listener, void* data)
{
    SwitchDevice* device = nullptr;
    device = wl_container_of(listener, device, toggle);
    auto* event = static_cast<wlr_switch_toggle_event*>(data);
    char const* type = event->switch_type == WLR_SWITCH_TYPE_LID ? "lid"
        : event->switch_type == WLR_SWITCH_TYPE_TABLET_MODE ? "tablet-mode"
                                                            : "keypad-slide";
    dbgln("WindowServer: switch {} {}", type, event->switch_state == WLR_SWITCH_STATE_ON ? "on" : "off");
    notify_activity(*device->server);
}

static void switch_destroy(wl_listener* listener, void*)
{
    SwitchDevice* device = nullptr;
    device = wl_container_of(listener, device, destroy);
    wl_list_remove(&device->toggle.link);
    wl_list_remove(&device->destroy.link);
    remove_owned(device->server->switch_objects, device);
}

// zwp-tablet-v2 (pen/tablet). wlroots reports tool events through the cursor; the
// compositor wraps each tablet/tool in its v2 counterpart and forwards surface
// coordinates. The tool's position is normalised [0,1] over the output layout.
static wlr_tablet_v2_tablet* v2_tablet_for(Server& server, wlr_tablet* tablet)
{
    if (!tablet || !server.tablet_manager)
        return nullptr;
    auto key = reinterpret_cast<u64>(tablet);
    if (auto it = server.tablet_map.find(key); it != server.tablet_map.end())
        return it->value;
    auto* v2 = wlr_tablet_create(server.tablet_manager, server.seat, &tablet->base);
    server.tablet_map.set(key, v2);
    return v2;
}

static wlr_tablet_v2_tablet_tool* v2_tablet_tool_for(Server& server, wlr_tablet_tool* tool)
{
    if (!tool || !server.tablet_manager)
        return nullptr;
    auto key = reinterpret_cast<u64>(tool);
    if (auto it = server.tablet_tool_map.find(key); it != server.tablet_tool_map.end())
        return it->value;
    auto* v2 = wlr_tablet_tool_create(server.tablet_manager, server.seat, tool);
    server.tablet_tool_map.set(key, v2);
    return v2;
}

static void tablet_tool_layout_position(Server& server, double x, double y, double* lx, double* ly)
{
    wlr_box box { };
    wlr_output_layout_get_box(server.output_layout, nullptr, &box);
    *lx = box.x + x * box.width;
    *ly = box.y + y * box.height;
}

// Tablet routing, shared by real pen/tablet devices (via the cursor) and the
// harness's virtual tablet. Coordinates are in the output layout.
static void handle_tablet_proximity(Server& server, wlr_tablet* tablet, wlr_tablet_tool* tool, bool in, double lx, double ly)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    // wlroots' proximity_in dereferences the surface, so only send it when the
    // pen is over a client surface; otherwise the tool stays out of proximity.
    wlr_surface* surface = nullptr;
    if (in) {
        double sx = 0;
        double sy = 0;
        if (auto* toplevel = toplevel_at(server, lx, ly, &surface, &sx, &sy))
            focus_toplevel(server, toplevel);
    }

    if (in && surface) {
        wlr_tablet_v2_tablet_tool_notify_proximity_in(v2_tool, v2_tablet_for(server, tablet), surface);
        dbgln("WindowServer: tablet tool in proximity");
    } else {
        wlr_tablet_v2_tablet_tool_notify_proximity_out(v2_tool);
        dbgln("WindowServer: tablet tool out of proximity");
    }
    notify_activity(server);
}

static void handle_tablet_motion(Server& server, wlr_tablet_tool* tool, double lx, double ly)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    toplevel_at(server, lx, ly, &surface, &sx, &sy);
    wlr_tablet_v2_tablet_tool_notify_motion(v2_tool, sx, sy);
    notify_activity(server);
}

static void handle_tablet_axis(Server& server, wlr_tablet_tool* tool, double pressure, double distance, double tilt_x, double tilt_y, double rotation, double slider, double wheel)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    wlr_tablet_v2_tablet_tool_notify_pressure(v2_tool, pressure);
    wlr_tablet_v2_tablet_tool_notify_distance(v2_tool, distance);
    wlr_tablet_v2_tablet_tool_notify_tilt(v2_tool, tilt_x, tilt_y);
    wlr_tablet_v2_tablet_tool_notify_rotation(v2_tool, rotation);
    wlr_tablet_v2_tablet_tool_notify_slider(v2_tool, slider);
    if (wheel != 0)
        wlr_tablet_v2_tablet_tool_notify_wheel(v2_tool, wheel, 0);
    notify_activity(server);
}

static void handle_tablet_tip(Server& server, wlr_tablet_tool* tool, bool down)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    if (down)
        wlr_tablet_v2_tablet_tool_notify_down(v2_tool);
    else
        wlr_tablet_v2_tablet_tool_notify_up(v2_tool);
    notify_activity(server);
}

static void handle_tablet_button(Server& server, wlr_tablet_tool* tool, uint32_t button, bool pressed)
{
    auto* v2_tool = v2_tablet_tool_for(server, tool);
    if (!v2_tool)
        return;
    wlr_tablet_v2_tablet_tool_notify_button(v2_tool, button,
        pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
    notify_activity(server);
}

static void tablet_tool_axis(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet_tool_axis);
    auto* event = static_cast<wlr_tablet_tool_axis_event*>(data);
    double lx = 0;
    double ly = 0;
    tablet_tool_layout_position(*server, event->x, event->y, &lx, &ly);
    if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y))
        handle_tablet_motion(*server, event->tool, lx, ly);
    if (event->updated_axes & (WLR_TABLET_TOOL_AXIS_PRESSURE | WLR_TABLET_TOOL_AXIS_DISTANCE
            | WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y
            | WLR_TABLET_TOOL_AXIS_ROTATION | WLR_TABLET_TOOL_AXIS_SLIDER | WLR_TABLET_TOOL_AXIS_WHEEL))
        handle_tablet_axis(*server, event->tool, event->pressure, event->distance, event->tilt_x, event->tilt_y, event->rotation, event->slider, event->wheel_delta);
}

static void tablet_tool_proximity(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet_tool_proximity);
    auto* event = static_cast<wlr_tablet_tool_proximity_event*>(data);
    double lx = 0;
    double ly = 0;
    tablet_tool_layout_position(*server, event->x, event->y, &lx, &ly);
    handle_tablet_proximity(*server, event->tablet, event->tool, event->state == WLR_TABLET_TOOL_PROXIMITY_IN, lx, ly);
}

static void tablet_tool_tip(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet_tool_tip);
    auto* event = static_cast<wlr_tablet_tool_tip_event*>(data);
    handle_tablet_tip(*server, event->tool, event->state == WLR_TABLET_TOOL_TIP_DOWN);
}

static void tablet_tool_button(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, tablet_tool_button);
    auto* event = static_cast<wlr_tablet_tool_button_event*>(data);
    handle_tablet_button(*server, event->tool, event->button, event->state == WLR_BUTTON_PRESSED);
}

// zwp-tablet-pad-v2: express keys and ring/strip controls.
static void tablet_pad_button(wl_listener* listener, void* data)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, button);
    auto* event = static_cast<wlr_tablet_pad_button_event*>(data);
    if (pad->v2_pad)
        wlr_tablet_v2_tablet_pad_notify_button(pad->v2_pad, event->button, event->time_msec,
            event->state == WLR_BUTTON_PRESSED ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                                               : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
}

static void tablet_pad_ring(wl_listener* listener, void* data)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, ring);
    auto* event = static_cast<wlr_tablet_pad_ring_event*>(data);
    if (pad->v2_pad)
        wlr_tablet_v2_tablet_pad_notify_ring(pad->v2_pad, event->ring, event->position,
            event->source == WLR_TABLET_PAD_RING_SOURCE_FINGER, event->time_msec);
}

static void tablet_pad_strip(wl_listener* listener, void* data)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, strip);
    auto* event = static_cast<wlr_tablet_pad_strip_event*>(data);
    if (pad->v2_pad)
        wlr_tablet_v2_tablet_pad_notify_strip(pad->v2_pad, event->strip, event->position,
            event->source == WLR_TABLET_PAD_STRIP_SOURCE_FINGER, event->time_msec);
}

static void tablet_pad_destroy(wl_listener* listener, void*)
{
    TabletPad* pad = nullptr;
    pad = wl_container_of(listener, pad, destroy);
    wl_list_remove(&pad->button.link);
    wl_list_remove(&pad->ring.link);
    wl_list_remove(&pad->strip.link);
    wl_list_remove(&pad->destroy.link);
    remove_owned(pad->server->tablet_pads, pad);
}

// --- XWayland (X11 clients) ------------------------------------------------
// X11 clients draw their own decorations, so the compositor manages content,
// position, focus and stacking only. Enabled by [Compositor] XWayland.

static X11Toplevel* x11_toplevel_for_wlr_surface(Server& server, wlr_surface* surface)
{
    for (auto& toplevel : server.x11_toplevels) {
        if (toplevel->xsurface && toplevel->xsurface->surface == surface)
            return toplevel.ptr();
    }
    return nullptr;
}

// The X11 (Xwayland) surface under a layout point, if any. Layer surfaces and
// xdg toplevels are handled elsewhere, so only X11 surfaces are considered.
static bool x11_surface_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy)
{
    if (!server.xwayland)
        return false;
    auto* node = wlr_scene_node_at(&server.scene->tree.node, lx, ly, sx, sy);
    if (!node || node->type != WLR_SCENE_NODE_BUFFER)
        return false;
    auto* scene_surface = wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
    if (!scene_surface || !wlr_xwayland_surface_try_from_wlr_surface(scene_surface->surface))
        return false;
    *surface = scene_surface->surface;
    return true;
}

// Override-redirect X11 windows (menus, tooltips) are transient children; the X
// server's stacking is not mirrored here, so keep them above their parent when
// the parent is raised. docs/20-xwayland-next-steps.md item 4.
static void x11_raise_overrides_above(X11Toplevel& parent)
{
    for (auto& child : parent.server->x11_toplevels) {
        if (!child->mapped || !child->xsurface)
            continue;
        // Override-redirect menus/tooltips follow X's parent; managed transients
        // (dialogs) use the tracked transient_parent.
        bool const is_child = child->override_redirect
            ? (parent.xsurface && child->xsurface->parent == parent.xsurface)
            : (child->transient_parent == &parent);
        if (is_child)
            wlr_scene_node_raise_to_top(&child->scene_tree->node);
    }
}

// Keep _NET_WM_STATE_ABOVE windows on top after another window is raised.
static void x11_raise_above_windows(Server& server)
{
    for (auto& toplevel : server.x11_toplevels) {
        if (toplevel->mapped && toplevel->always_on_top)
            wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    }
}

static void focus_x11_toplevel(Server& server, X11Toplevel* toplevel)
{
    if (!toplevel || !toplevel->xsurface || !toplevel->xsurface->surface)
        return;
    // Override-redirect windows (menus, tooltips, drag icons) never take focus.
    if (toplevel->override_redirect)
        return;
    wlr_surface* surface = toplevel->xsurface->surface;
    if (server.seat->keyboard_state.focused_surface == surface)
        return;

    if (auto* previous = server.seat->keyboard_state.focused_surface) {
        if (auto* xdg = wlr_xdg_toplevel_try_from_wlr_surface(previous)) {
            wlr_xdg_toplevel_set_activated(xdg, false);
            for (auto& other : server.toplevels) {
                if (other->xdg_toplevel && other->xdg_toplevel->base->surface == previous) {
                    other->frame_active = false;
                    toplevel_update_frame(*other);
                    break;
                }
            }
        }
    }
    if (server.focused_x11 && server.focused_x11 != toplevel) {
        wlr_xwayland_surface_activate(server.focused_x11->xsurface, false);
        server.focused_x11->frame_active = false;
        if (server.focused_x11->frame_decorated)
            x11_update_frame(*server.focused_x11);
    }
    server.focused_x11 = toplevel;
    // Feed the Alt-Tab switcher's most-recently-focused ordering (see
    // Toplevel::focus_order; X11Toplevel gained the same field).
    toplevel->focus_order = ++g_focus_order_counter;

    server.keyboard_focus_layer = nullptr;
    stop_key_repeat();
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    // Keep this window's override-redirect children (menus/tooltips) above it,
    // and any always-on-top windows at the very top.
    x11_raise_overrides_above(*toplevel);
    x11_raise_above_windows(server);
    toplevel->frame_active = true;
    wlr_xwayland_surface_activate(toplevel->xsurface, true);
    if (toplevel->frame_decorated)
        x11_update_frame(*toplevel);
    // Focusing a window clears its demands-attention state.
    toplevel->demands_attention = false;
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

// The client surface (xdg x11 content or X11) under a layout point.
static wlr_surface* window_surface_at(Server& server, double lx, double ly, double* sx, double* sy)
{
    wlr_surface* surface = nullptr;
    toplevel_at(server, lx, ly, &surface, sx, sy);
    if (!surface)
        x11_surface_at(server, lx, ly, &surface, sx, sy);
    return surface;
}

// Focus the window (xdg or X11) under a layout point.
static void focus_window_at(Server& server, double lx, double ly)
{
    double sx = 0;
    double sy = 0;
    wlr_surface* surface = nullptr;
    if (auto* toplevel = toplevel_at(server, lx, ly, &surface, &sx, &sy)) {
        focus_toplevel(server, toplevel);
        return;
    }
    if (x11_surface_at(server, lx, ly, &surface, &sx, &sy)) {
        if (auto* x11 = x11_toplevel_for_wlr_surface(server, surface))
            focus_x11_toplevel(server, x11);
    }
}

static void x11_surface_map(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, surface_map);
    auto& server = *toplevel->server;
    toplevel->mapped = true;
    toplevel->title = ByteString { toplevel->xsurface->title ? StringView { toplevel->xsurface->title, strlen(toplevel->xsurface->title) } : StringView { } };
    // A newly mapped X11 window joins the current workspace stack (its scene node
    // is enabled only if that stack is the current one).
    WindowManager::on_x11_toplevel_mapped(*toplevel);
    wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    x11_update_frame(*toplevel);
    // Place it (frame at the requested position) once the frame size is known, so
    // the whole window — titlebar included — is on-screen.
    x11_place_frame(*toplevel);
    // WM_CLASS / _NET_WM_ICON / _NET_WM_STATE may have been set before the
    // listeners were attached.
    x11_update_class(*toplevel);
    x11_fetch_icon(*toplevel);
    x11_update_hints(*toplevel);
    dbgln("WindowServer: X11 window mapped '{}' at {},{}", toplevel->xsurface->title ? toplevel->xsurface->title : "", toplevel->xsurface->x, toplevel->xsurface->y);
    // Override-redirect windows (menus, tooltips) are unmanaged: no frame, no
    // focus, no Taskbar entry.
    if (!toplevel->override_redirect) {
        // _NET_WM_STATE_SKIP_TASKBAR: a normal window, but no Taskbar entry.
        if (!toplevel->skip_taskbar && toplevel->wm_id == 0)
            toplevel->wm_id = g_next_wm_id++;
        focus_x11_toplevel(server, toplevel);
        if (toplevel->always_on_top)
            x11_raise_above_windows(server);
        if (toplevel->wm_id != 0)
            WindowManager::emit_window_state(*toplevel);
    } else {
        // They are transient and never workspace-hid (docs/20 item 4).
        dbgln("WindowServer: X11 override-redirect window mapped '{}' at {},{}", toplevel->title, toplevel->xsurface->x, toplevel->xsurface->y);
    }
}

static void x11_surface_unmap(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, surface_unmap);
    toplevel->mapped = false;
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
}

static void x11_surface_associate(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, associate);
    auto* xsurface = toplevel->xsurface;
    if (!xsurface->surface)
        return;
    // Re-associating would add the same listeners twice (corrupting the list), so
    // drop any previous ones first.
    if (toplevel->surface_listeners_attached) {
        wl_list_remove(&toplevel->surface_map.link);
        wl_list_remove(&toplevel->surface_unmap.link);
        toplevel->surface_listeners_attached = false;
    }
    toplevel->scene = wlr_scene_surface_create(toplevel->scene_tree, xsurface->surface);
    toplevel->surface_map.notify = x11_surface_map;
    wl_signal_add(&xsurface->surface->events.map, &toplevel->surface_map);
    toplevel->surface_unmap.notify = x11_surface_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &toplevel->surface_unmap);
    toplevel->listened_surface = xsurface->surface;
    toplevel->surface_listeners_attached = true;
}

static void x11_surface_dissociate(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, dissociate);
    toplevel->mapped = false;
    // The surface is going away: detach our map/unmap listeners from it.
    if (toplevel->surface_listeners_attached) {
        wl_list_remove(&toplevel->surface_map.link);
        wl_list_remove(&toplevel->surface_unmap.link);
        toplevel->surface_listeners_attached = false;
        toplevel->listened_surface = nullptr;
    }
    if (toplevel->scene) {
        wlr_scene_node_destroy(&toplevel->scene->buffer->node);
        toplevel->scene = nullptr;
    }
}

static void x11_surface_destroy(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, destroy);
    auto& server = *toplevel->server;
    // Detach every listener we added to the Xwayland surface: wlroots' destroy
    // asserts that all of these event listener lists are empty, so leaving any
    // attached aborts the whole compositor (e.g. when an xterm exits).
    if (toplevel->surface_listeners_attached) {
        wl_list_remove(&toplevel->surface_map.link);
        wl_list_remove(&toplevel->surface_unmap.link);
        toplevel->surface_listeners_attached = false;
        toplevel->listened_surface = nullptr;
    }
    wl_list_remove(&toplevel->associate.link);
    wl_list_remove(&toplevel->dissociate.link);
    wl_list_remove(&toplevel->request_configure.link);
    wl_list_remove(&toplevel->request_close.link);
    wl_list_remove(&toplevel->request_activate.link);
    wl_list_remove(&toplevel->set_title.link);
    wl_list_remove(&toplevel->set_class.link);
    wl_list_remove(&toplevel->set_icon.link);
    wl_list_remove(&toplevel->set_hints.link);
    wl_list_remove(&toplevel->request_skip_taskbar.link);
    wl_list_remove(&toplevel->request_demands_attention.link);
    wl_list_remove(&toplevel->destroy.link);
    if (server.focused_x11 == toplevel)
        server.focused_x11 = nullptr;
    if (server.grabbed_x11 == toplevel)
        server.grabbed_x11 = nullptr;
    if (server.pressed_button_x11 == toplevel)
        server.pressed_button_x11 = nullptr;
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_removed(*toplevel);
    if (toplevel->frame_buffer)
        toplevel->frame_buffer = nullptr; // destroyed with the scene tree
    if (toplevel->scene_tree) {
        wlr_scene_node_destroy(&toplevel->scene_tree->node);
        toplevel->scene_tree = nullptr;
    }
    // Avoid dangling transient-parent pointers (and our surface back-reference).
    for (auto& other : server.x11_toplevels) {
        if (other->transient_parent == toplevel)
            other->transient_parent = nullptr;
    }
    if (toplevel->xsurface)
        toplevel->xsurface->data = nullptr;
    remove_owned(server.x11_toplevels, toplevel);
}

// Place the window's frame origin at the client's requested position, keeping the
// whole window — compositor frame included — on the primary output. The scene
// tree is the frame origin (as for xdg toplevels): the frame is drawn at (0,0)
// and the client content is inset by the frame inset.
static void x11_place_frame(X11Toplevel& toplevel)
{
    if (!toplevel.scene_tree || !toplevel.xsurface)
        return;
    auto& server = *toplevel.server;
    int x = toplevel.xsurface->x;
    int y = toplevel.xsurface->y;
    int const w = toplevel.frame_size.width() > 0 ? toplevel.frame_size.width() : toplevel.xsurface->width;
    int const h = toplevel.frame_size.height() > 0 ? toplevel.frame_size.height() : toplevel.xsurface->height;
    wlr_box workarea { };
    wlr_output_layout_get_box(server.output_layout, server.primary_output, &workarea);
    if (!wlr_box_empty(&workarea)) {
        x = max(x, workarea.x);
        y = max(y, workarea.y);
        if (w <= workarea.width)
            x = min(x, workarea.x + workarea.width - w);
        if (h <= workarea.height)
            y = min(y, workarea.y + workarea.height - h);
    }
    wlr_scene_node_set_position(&toplevel.scene_tree->node, x, y);
}

static void x11_request_configure(wl_listener* listener, void* data)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_configure);
    auto* event = static_cast<wlr_xwayland_surface_configure_event*>(data);
    wlr_xwayland_surface_configure(toplevel->xsurface, event->x, event->y, event->width, event->height);
    x11_update_frame(*toplevel);
    if (toplevel->mapped)
        x11_place_frame(*toplevel);
}

static void x11_request_close(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_close);
    wlr_xwayland_surface_close(toplevel->xsurface);
}

static void x11_request_activate(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_activate);
    if (!toplevel->override_redirect)
        focus_x11_toplevel(*toplevel->server, toplevel);
}

static void x11_set_title(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_title);
    dbgln("WindowServer: X11 window '{}'", toplevel->xsurface->title ? toplevel->xsurface->title : "");
    toplevel->title = ByteString { toplevel->xsurface->title ? StringView { toplevel->xsurface->title, strlen(toplevel->xsurface->title) } : StringView { } };
    if (toplevel->frame_decorated)
        x11_update_frame(*toplevel);
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
}

// WM_CLASS (res_name/res_class): the app identity, used for grouping. Kept on the
// toplevel so the WM layer needs no wlroots/xcb types.
static void x11_update_class(X11Toplevel& toplevel)
{
    auto* xsurface = toplevel.xsurface;
    char const* instance = xsurface->instance;
    char const* cls = xwayland_surface_class(xsurface);
    toplevel.wm_instance = ByteString { instance ? StringView { instance, strlen(instance) } : StringView { } };
    toplevel.wm_class = ByteString { cls ? StringView { cls, strlen(cls) } : StringView { } };
    dbgln("WindowServer: X11 window {} class='{}' instance='{}'", toplevel.wm_id, toplevel.wm_class, toplevel.wm_instance);
}

static void x11_set_class(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_class);
    x11_update_class(*toplevel);
}

// _NET_WM_STATE hints and transiency (docs/20 item 7). wlroots parses the
// property into the surface fields; mirror the ones the WM acts on.
static void x11_update_hints(X11Toplevel& toplevel)
{
    auto* xsurface = toplevel.xsurface;
    if (!xsurface)
        return;
    toplevel.skip_taskbar = xsurface->skip_taskbar;
    toplevel.always_on_top = xsurface->above;
    toplevel.demands_attention = xsurface->demands_attention;
    toplevel.modal = xsurface->modal;
    toplevel.transient_parent = nullptr;
    if (xsurface->parent && xsurface->parent->data)
        toplevel.transient_parent = static_cast<X11Toplevel*>(xsurface->parent->data);
    dbgln("WindowServer: X11 window {} hints: skip_taskbar={} above={} demands_attention={} modal={} transient={}",
        toplevel.wm_id, toplevel.skip_taskbar, toplevel.always_on_top, toplevel.demands_attention, toplevel.modal,
        toplevel.transient_parent ? toplevel.transient_parent->wm_id : 0);
}

static void x11_request_skip_taskbar(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_skip_taskbar);
    x11_update_hints(*toplevel);
    // A window that now skips the Taskbar leaves the window list.
    if (toplevel->skip_taskbar && toplevel->wm_id != 0)
        WindowManager::emit_window_removed(*toplevel);
}

static void x11_request_demands_attention(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, request_demands_attention);
    x11_update_hints(*toplevel);
    if (toplevel->wm_id != 0)
        WindowManager::emit_window_state(*toplevel);
}

// _NET_WM_ICON: wlroots exposes the property (wlr_xwayland_surface_fetch_icon);
// pick a frame and wrap it in a Gfx::Bitmap so the Taskbar button and the Alt-Tab
// switcher can draw it. _NET_WM_ICON pixels are 0xAARRGGBB, i.e. B,G,R,A in
// memory (little-endian), which is exactly BitmapFormat::BGRA8888.

// Older clients (e.g. xterm) set their window icon via the legacy WM_HINTS
// icon_pixmap (+ optional 1-bit mask), not _NET_WM_ICON. Read it over the xwm
// connection (wlr_xwayland_get_xwm_connection) and build a Gfx::Bitmap.
static void x11_fetch_wm_hints_icon(X11Toplevel& toplevel)
{
    auto* xsurface = toplevel.xsurface;
    if (!xsurface || !xsurface->hints)
        return;
    if (!(xsurface->hints->flags & XCB_ICCCM_WM_HINT_ICON_PIXMAP))
        return;
    xcb_pixmap_t const pixmap = xsurface->hints->icon_pixmap;
    if (pixmap == XCB_NONE)
        return;
    xcb_pixmap_t const mask = (xsurface->hints->flags & XCB_ICCCM_WM_HINT_ICON_MASK) ? xsurface->hints->icon_mask : XCB_NONE;

    auto* connection = (toplevel.server && toplevel.server->xwayland) ? wlr_xwayland_get_xwm_connection(toplevel.server->xwayland) : nullptr;
    if (!connection)
        return;

    xcb_get_geometry_reply_t* geometry = xcb_get_geometry_reply(connection, xcb_get_geometry(connection, pixmap), nullptr);
    if (!geometry)
        return;
    int const width = geometry->width;
    int const height = geometry->height;
    uint8_t const depth = geometry->depth;
    free(geometry);
    if (width <= 0 || height <= 0 || width > 256 || height > 256)
        return;

    xcb_get_image_reply_t* image = xcb_get_image_reply(connection, xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, pixmap, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height), ~0u), nullptr);
    if (!image)
        return;
    uint8_t const* data = xcb_get_image_data(image);

    xcb_get_image_reply_t* mask_image = nullptr;
    uint8_t const* mask_data = nullptr;
    if (mask != XCB_NONE) {
        mask_image = xcb_get_image_reply(connection, xcb_get_image(connection, XCB_IMAGE_FORMAT_Z_PIXMAP, mask, 0, 0, static_cast<uint16_t>(width), static_cast<uint16_t>(height), ~0u), nullptr);
        if (mask_image)
            mask_data = xcb_get_image_data(mask_image);
    }

    if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { width, height }); !bitmap.is_error()) {
        auto& created = *bitmap.value();
        int const one_bit_stride = ((width + 31) / 32) * 4; // 1-bit rows pad to 32 bits
        for (int y = 0; y < height; ++y) {
            auto* dst = created.scanline_u8(y);
            for (int x = 0; x < width; ++x) {
                uint8_t r = 0;
                uint8_t g = 0;
                uint8_t b = 0;
                if (depth == 1) {
                    uint8_t const bit = (data[static_cast<size_t>(y) * one_bit_stride + x / 8] >> (x % 8)) & 1;
                    r = g = b = bit ? 0x10 : 0xf0; // foreground / background of the bitmap
                } else if (depth == 24 || depth == 32) {
                    uint8_t const* p = data + (static_cast<size_t>(y) * width + x) * 4; // 32 bits/pixel
                    b = p[0];
                    g = p[1];
                    r = p[2];
                } else {
                    continue;
                }
                uint8_t alpha = 255;
                if (mask_data) {
                    uint8_t const m = (mask_data[static_cast<size_t>(y) * one_bit_stride + x / 8] >> (x % 8)) & 1;
                    alpha = m ? 255 : 0;
                }
                dst[x * 4 + 0] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = alpha;
            }
        }
        toplevel.icon = bitmap.release_value();
        dbgln("WindowServer: X11 window '{}' WM_HINTS icon {}x{} (wm_id={})", toplevel.title, width, height, toplevel.wm_id);
    }
    free(image);
    if (mask_image)
        free(mask_image);
}

static void x11_fetch_icon(X11Toplevel& toplevel)
{
    if (!toplevel.xsurface)
        return;

    bool have_icon = false;
    xcb_ewmh_get_wm_icon_reply_t reply;
    if (wlr_xwayland_surface_fetch_icon(toplevel.xsurface, &reply)) {
        unsigned best_width = 0;
        unsigned best_height = 0;
        uint32_t const* best_pixels = nullptr;
        int best_rank = -1;
        auto it = xcb_ewmh_get_wm_icon_iterator(&reply);
        while (it.rem > 0) {
            unsigned const w = it.width;
            unsigned const h = it.height;
            int rank;
            if (w == 16 && h == 16)
                rank = 1 << 20; // the Taskbar/switcher size
            else if (w <= 64 && h <= 64)
                rank = static_cast<int>(w * h);
            else
                rank = 0;
            if (it.data && rank > best_rank) {
                best_rank = rank;
                best_width = w;
                best_height = h;
                best_pixels = it.data;
            }
            xcb_ewmh_get_wm_icon_next(&it);
        }

        if (best_pixels && best_width > 0 && best_height > 0) {
            if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { static_cast<int>(best_width), static_cast<int>(best_height) }); !bitmap.is_error()) {
                auto& created = *bitmap.value();
                for (unsigned y = 0; y < best_height; ++y)
                    memcpy(created.scanline(static_cast<int>(y)), best_pixels + static_cast<size_t>(y) * best_width, static_cast<size_t>(best_width) * 4);
                toplevel.icon = bitmap.release_value();
                have_icon = true;
                dbgln("WindowServer: X11 window '{}' icon {}x{} (wm_id={})", toplevel.title, best_width, best_height, toplevel.wm_id);
            }
        }
        xcb_ewmh_get_wm_icon_reply_wipe(&reply);
    }

    // Fall back to the legacy WM_HINTS icon (xterm and friends).
    if (!have_icon && toplevel.server && toplevel.xsurface && toplevel.xsurface->hints
        && (toplevel.xsurface->hints->flags & XCB_ICCCM_WM_HINT_ICON_PIXMAP)
        && toplevel.xsurface->hints->icon_pixmap != XCB_NONE)
        x11_fetch_wm_hints_icon(toplevel);

    if (toplevel.icon) {
        WindowManager::emit_window_icon(toplevel);
        // Repaint the titlebar so it shows the icon.
        if (toplevel.frame_decorated)
            x11_update_frame(toplevel);
    }
}

static void x11_set_icon(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_icon);
    x11_fetch_icon(*toplevel);
}

// WM_HINTS changed (e.g. the legacy icon pixmap): (re)read the icon.
static void x11_set_hints(wl_listener* listener, void*)
{
    X11Toplevel* toplevel = nullptr;
    toplevel = wl_container_of(listener, toplevel, set_hints);
    x11_fetch_icon(*toplevel);
}

static void server_new_xwayland_surface(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, new_xwayland_surface);
    auto* xsurface = static_cast<wlr_xwayland_surface*>(data);

    auto toplevel = make<X11Toplevel>();
    toplevel->server = server;
    toplevel->xsurface = xsurface;
    toplevel->override_redirect = xsurface->override_redirect;
    // The default icon (a big "X") until the client supplies its own: it shows on the
    // titlebar, the Alt-Tab switcher and the Taskbar button.
    toplevel->icon = g_x11_default_icon;
    toplevel->scene_tree = wlr_scene_tree_create(server->windows_tree);
    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
    xsurface->data = toplevel.ptr();

    toplevel->associate.notify = x11_surface_associate;
    wl_signal_add(&xsurface->events.associate, &toplevel->associate);
    toplevel->dissociate.notify = x11_surface_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &toplevel->dissociate);
    toplevel->destroy.notify = x11_surface_destroy;
    wl_signal_add(&xsurface->events.destroy, &toplevel->destroy);
    toplevel->request_configure.notify = x11_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &toplevel->request_configure);
    toplevel->request_close.notify = x11_request_close;
    wl_signal_add(&xsurface->events.request_close, &toplevel->request_close);
    toplevel->request_activate.notify = x11_request_activate;
    wl_signal_add(&xsurface->events.request_activate, &toplevel->request_activate);
    toplevel->set_title.notify = x11_set_title;
    wl_signal_add(&xsurface->events.set_title, &toplevel->set_title);
    toplevel->set_class.notify = x11_set_class;
    wl_signal_add(&xsurface->events.set_class, &toplevel->set_class);
    toplevel->set_icon.notify = x11_set_icon;
    wl_signal_add(&xsurface->events.set_icon, &toplevel->set_icon);
    toplevel->set_hints.notify = x11_set_hints;
    wl_signal_add(&xsurface->events.set_hints, &toplevel->set_hints);
    toplevel->request_skip_taskbar.notify = x11_request_skip_taskbar;
    wl_signal_add(&xsurface->events.request_skip_taskbar, &toplevel->request_skip_taskbar);
    toplevel->request_demands_attention.notify = x11_request_demands_attention;
    wl_signal_add(&xsurface->events.request_demands_attention, &toplevel->request_demands_attention);

    // WM_CLASS may already be set by the time we associate.
    x11_update_class(*toplevel);

    server->x11_toplevels.append(move(toplevel));
}

static void xwayland_ready(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, xwayland_ready);
    server->xwayland_started = true;
    // Now that xwm exists, report the workarea (panels' exclusive zones).
    update_xwayland_workareas(*server);
    dbgln("WindowServer: Xwayland ready (DISPLAY={})", server->xwayland->display_name);
}

// --- X11 server-side decoration frames -------------------------------------

// Render the SerenityOS frame around an X11 window, unless the client draws its
// own decorations (Motif hints) or is an override-redirect window.
static void x11_update_frame(X11Toplevel& toplevel)
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
        if (toplevel.frame_buffer)
            wlr_scene_node_set_enabled(&toplevel.frame_buffer->node, false);
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

    auto* buffer = bitmap_buffer_create(move(bitmap));
    if (!toplevel.frame_buffer) {
        toplevel.frame_buffer = wlr_scene_buffer_create(toplevel.scene_tree, buffer);
        wlr_scene_node_set_position(&toplevel.frame_buffer->node, 0, 0);
    } else {
        wlr_scene_buffer_set_buffer(toplevel.frame_buffer, buffer);
    }
    wlr_scene_buffer_set_dest_size(toplevel.frame_buffer, frame_size.width(), frame_size.height());
    // The scene tree is the *frame* origin (as for xdg toplevels): the frame is
    // drawn at (0,0) and the client content is inset by the frame, so a window
    // that asks to be at (0,0) shows its titlebar on-screen.
    wlr_scene_node_set_position(&toplevel.frame_buffer->node, 0, 0);
    wlr_scene_node_set_enabled(&toplevel.frame_buffer->node, true);
    use_nearest_filter(&toplevel.frame_buffer->node);
    wlr_buffer_drop(buffer);
    if (toplevel.scene)
        wlr_scene_node_set_position(&toplevel.scene->buffer->node, toplevel.frame_left, toplevel.frame_top);
    dbgln("WindowServer: X11 frame '{}' {}x{} (content {}x{})", title, frame_size.width(), frame_size.height(), content_width, content_height);
}

static FrameHit x11_hit_test_frame(Server& server, double x, double y)
{
    FrameHit hit;
    for (size_t index = server.x11_toplevels.size(); index-- > 0;) {
        auto& toplevel = *server.x11_toplevels[index];
        if (!toplevel.mapped || !toplevel.frame_decorated)
            continue;
        auto* node = &toplevel.scene_tree->node;
        Gfx::IntRect frame_rect {
            static_cast<int>(node->x), static_cast<int>(node->y),
            toplevel.frame_size.width(), toplevel.frame_size.height()
        };
        if (frame_rect.is_empty() || !frame_rect.contains({ static_cast<int>(x), static_cast<int>(y) }))
            continue;
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

static void x11_begin_interactive(X11Toplevel& toplevel, CursorMode mode, uint32_t edges)
{
    auto& server = *toplevel.server;
    server.grabbed_toplevel = nullptr;
    server.grabbed_x11 = &toplevel;
    server.cursor_mode = mode;
    int width = toplevel.xsurface->width;
    int height = toplevel.xsurface->height;
    if (mode == CursorMode::Move) {
        server.grab_x = server.cursor->x - toplevel.scene_tree->node.x;
        server.grab_y = server.cursor->y - toplevel.scene_tree->node.y;
    } else {
        // The scene tree is the frame origin; the frame edge is dragged.
        int const frame_w = toplevel.frame_size.width() > 0 ? toplevel.frame_size.width() : width;
        int const frame_h = toplevel.frame_size.height() > 0 ? toplevel.frame_size.height() : height;
        double border_x = toplevel.scene_tree->node.x + ((edges & WLR_EDGE_RIGHT) ? frame_w : 0);
        double border_y = toplevel.scene_tree->node.y + ((edges & WLR_EDGE_BOTTOM) ? frame_h : 0);
        server.grab_x = server.cursor->x - border_x;
        server.grab_y = server.cursor->y - border_y;
        server.grab_box = { static_cast<int>(toplevel.scene_tree->node.x), static_cast<int>(toplevel.scene_tree->node.y), frame_w, frame_h };
        server.resize_edges = edges;
    }
    wlr_cursor_set_xcursor(server.cursor, server.cursor_manager, mode == CursorMode::Move ? "grabbing" : resize_cursor_name(edges));
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

static bool handle_x11_frame_press(Server& server, uint32_t button)
{
    auto hit = x11_hit_test_frame(server, server.cursor->x, server.cursor->y);
    auto* toplevel = hit.x11_toplevel;
    if (!toplevel)
        return false;
    if (button == BTN_LEFT && hit.region == FrameRegion::Button) {
        server.pressed_button_x11 = toplevel;
        server.pressed_button_x11_index = hit.button_index;
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

static void handle_x11_frame_release(Server& server, uint32_t button)
{
    auto* toplevel = server.pressed_button_x11;
    if (!toplevel)
        return;
    server.pressed_button_x11 = nullptr;
    int index = server.pressed_button_x11_index;
    server.pressed_button_x11_index = -1;
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
static void update_x11_button_hover(Server& server, double x, double y)
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
static void hook_focus_x11(Server& server, X11Toplevel* toplevel)
{
    if (!toplevel)
        return;
    if (toplevel->xsurface->minimized)
        wlr_xwayland_surface_set_minimized(toplevel->xsurface, false);
    focus_x11_toplevel(server, toplevel);
}

static void hook_set_minimized_x11(Server&, X11Toplevel& toplevel, bool minimized)
{
    wlr_xwayland_surface_set_minimized(toplevel.xsurface, minimized);
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
    case WLR_INPUT_DEVICE_POINTER: {
        wlr_cursor_attach_input_device(server->cursor, device);
        server->pointer_devices.append(device);
        apply_mouse_input(*server);
        // Track the pointer so its touchpad gestures can be forwarded (R5).
        auto pointer = make<Pointer>();
        pointer->server = server;
        pointer->device = device;
        pointer->pointer = wlr_pointer_from_input_device(device);
        pointer->swipe_begin.notify = pointer_swipe_begin;
        wl_signal_add(&pointer->pointer->events.swipe_begin, &pointer->swipe_begin);
        pointer->swipe_update.notify = pointer_swipe_update;
        wl_signal_add(&pointer->pointer->events.swipe_update, &pointer->swipe_update);
        pointer->swipe_end.notify = pointer_swipe_end;
        wl_signal_add(&pointer->pointer->events.swipe_end, &pointer->swipe_end);
        pointer->pinch_begin.notify = pointer_pinch_begin;
        wl_signal_add(&pointer->pointer->events.pinch_begin, &pointer->pinch_begin);
        pointer->pinch_update.notify = pointer_pinch_update;
        wl_signal_add(&pointer->pointer->events.pinch_update, &pointer->pinch_update);
        pointer->pinch_end.notify = pointer_pinch_end;
        wl_signal_add(&pointer->pointer->events.pinch_end, &pointer->pinch_end);
        pointer->hold_begin.notify = pointer_hold_begin;
        wl_signal_add(&pointer->pointer->events.hold_begin, &pointer->hold_begin);
        pointer->hold_end.notify = pointer_hold_end;
        wl_signal_add(&pointer->pointer->events.hold_end, &pointer->hold_end);
        pointer->destroy.notify = pointer_destroy;
        wl_signal_add(&device->events.destroy, &pointer->destroy);
        server->pointer_objects.append(move(pointer));
        break;
    }
    case WLR_INPUT_DEVICE_SWITCH: {
        auto switch_device = make<SwitchDevice>();
        switch_device->server = server;
        switch_device->device = wlr_switch_from_input_device(device);
        switch_device->toggle.notify = switch_toggle;
        wl_signal_add(&switch_device->device->events.toggle, &switch_device->toggle);
        switch_device->destroy.notify = switch_destroy;
        wl_signal_add(&device->events.destroy, &switch_device->destroy);
        server->switch_objects.append(move(switch_device));
        break;
    }
    case WLR_INPUT_DEVICE_TABLET:
        // Route tablet tool events through the cursor; the v2 wrappers are made
        // lazily when the first event for a tool arrives.
        wlr_cursor_attach_input_device(server->cursor, device);
        break;
    case WLR_INPUT_DEVICE_TABLET_PAD: {
        auto pad = make<TabletPad>();
        pad->server = server;
        pad->pad = wlr_tablet_pad_from_input_device(device);
        pad->v2_pad = server->tablet_manager ? wlr_tablet_pad_create(server->tablet_manager, server->seat, device) : nullptr;
        pad->button.notify = tablet_pad_button;
        wl_signal_add(&pad->pad->events.button, &pad->button);
        pad->ring.notify = tablet_pad_ring;
        wl_signal_add(&pad->pad->events.ring, &pad->ring);
        pad->strip.notify = tablet_pad_strip;
        wl_signal_add(&pad->pad->events.strip, &pad->strip);
        pad->destroy.notify = tablet_pad_destroy;
        wl_signal_add(&device->events.destroy, &pad->destroy);
        server->tablet_pads.append(move(pad));
        break;
    }
    case WLR_INPUT_DEVICE_TOUCH:
        // Touch is routed through the cursor (touch_down/motion/up events), like
        // pointers; the compositor converts them to wlr_seat touch notifications.
        wlr_cursor_attach_input_device(server->cursor, device);
        server->touch_devices.append(device);
        break;
    default:
        break;
    }

    update_seat_capabilities(*server);
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
static void set_buffer_filter(struct wlr_scene_buffer* buffer, int, int, void*)
{
    // An integer scale is exact, so nearest-neighbour is a pixel-doubling no-op at
    // 1x and crisp at 2x; a fractional scale needs linear filtering.
    wlr_scene_buffer_set_filter_mode(buffer, output_scale_is_integral() ? WLR_SCALE_FILTER_NEAREST : WLR_SCALE_FILTER_BILINEAR);
}

static void use_nearest_filter(struct wlr_scene_node* node)
{
    wlr_scene_node_for_each_buffer(node, set_buffer_filter, nullptr);
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

// R6 presentation-time: mark the surfaces the upcoming frame displays on this
// output, so wlroots can deliver wp_presentation_feedback ("presented" events).
// The scene reports the buffers visible on the output in rendering order.
static void mark_buffer_presented(wlr_scene_buffer* buffer, int sx, int sy, void* data)
{
    (void)sx;
    (void)sy;
    auto* output = static_cast<wlr_output*>(data);
    if (auto* scene_surface = wlr_scene_surface_try_from_buffer(buffer))
        wlr_presentation_surface_textured_on_output(scene_surface->surface, output);
}

static void output_frame(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, frame);
    update_desktop_background(*output->server, *output);
    auto* scene_output = wlr_scene_get_scene_output(output->server->scene, output->output);
    if (!scene_output)
        return;
    wlr_scene_output_for_each_buffer(scene_output, mark_buffer_presented, output->output);
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
    auto& server = *output->server;
    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->request_state.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->commit.link);
    if (output->background)
        wlr_scene_node_destroy(&output->background->node);
    remove_owned(server.outputs, output);
    // R5: the InputCapture zones are the outputs.
    InputCapture::on_outputs_changed(server);
}

static void output_commit(wl_listener* listener, void*)
{
    Output* output = nullptr;
    output = wl_container_of(listener, output, commit);
    reconfigure_lock_surface(*output->server, output->output);
}

// --- HiDPI / fractional output scale ---------------------------------------
//
// Serenity's LibGUI renders on the CPU and its fonts are fixed bitmaps, so a
// high-DPI panel is made usable by scaling the output (integer by default, but
// fractional is supported: the compositor renders its own chrome at an integer
// supersample and lets the scene scale it down, native clients learn the exact
// scale from wp-fractional-scale-v1). The client-side backing-store scale is the
// libgui-wayland shim's job (it rasters at the output scale). See
// docs/13-native-session-and-aur-packaging.md.
//
// Auto rule: 2x when the panel's true DPI is >= 144, else 1x; panels that omit
// their physical size fall back to a resolution threshold.
static float auto_output_scale(struct wlr_output* output, struct wlr_output_mode* mode)
{
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

// Resolve the output scale: the SDE_OUTPUT_SCALE override wins, then the
// configured [Output] DefaultScale, then a saved layout, then the auto rule.
static float resolve_output_scale(struct wlr_output* output, struct wlr_output_mode* mode, Config::OutputSettings const* saved)
{
    if (auto const* env = getenv("SDE_OUTPUT_SCALE"); env && *env) {
        char* end = nullptr;
        double const value = strtod(env, &end);
        if (end != env && value >= 0.5)
            return static_cast<float>(value);
    }
    if (double const configured = Config::output().default_scale; configured > 0)
        return static_cast<float>(configured);
    if (saved && saved->scale > 0)
        return static_cast<float>(saved->scale);
    return auto_output_scale(output, mode);
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
    if (mode)
        wlr_output_state_set_mode(&state, mode);
    else if (saved && saved->width > 0 && saved->height > 0)
        // A saved size with no matching mode (e.g. the headless backend): request
        // a custom mode, which the backend honours (HiDPI captures need this).
        wlr_output_state_set_custom_mode(&state, saved->width, saved->height, 0);
    else if (auto* preferred = wlr_output_preferred_mode(wlr_output))
        wlr_output_state_set_mode(&state, preferred);
    // HiDPI / fractional scale (see resolve_output_scale).
    float const scale = resolve_output_scale(wlr_output, mode, saved);
    if (scale != 1.0f) {
        wlr_output_state_set_scale(&state, scale);
        dbgln("WindowServer: output '{}' scaling {}x ({})",
            wlr_output->name ? wlr_output->name : "?", scale, saved ? "saved layout" : "high-DPI");
    } else if (saved) {
        dbgln("WindowServer: output '{}' scale 1x (saved layout)", wlr_output->name ? wlr_output->name : "?");
    }
    wlr_output_commit_state(wlr_output, &state);
    // Compositor-drawn chrome (frames, overlays, the switcher) is rendered at the
    // output scale so it stays crisp on HiDPI panels.
    set_output_scale(wlr_output->scale);
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
    dbgln("WindowServer: output '{}' {}x{} @{}x", wlr_output->name ? wlr_output->name : "?", logical_size.width(), logical_size.height(), wlr_output->scale);
    server->outputs.append(move(output));
    update_desktop_background(*server, *server->outputs.last());
    arrange_layers(*server, wlr_output);
    output_manager_send_configuration(*server);
    // R4.5: keep screenlayout.ini in sync with the live layout (coalesced so a
    // startup burst of outputs writes the complete layout once).
    schedule_persist_screen_layout(*server);
    // R5: the InputCapture zones are the outputs.
    InputCapture::on_outputs_changed(*server);
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
        if (toplevel.frame_buffer)
            wlr_scene_node_set_enabled(&toplevel.frame_buffer->node, false);
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
        && frame_cache->icon == toplevel.icon.ptr()
        && frame_cache->scale == output_scale()
        && frame_cache->title == title_view)
        return;

    // Render the frame at the output scale so it stays crisp on HiDPI panels.
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
    // The frame bitmap is physical; present it at the logical frame size.
    wlr_scene_buffer_set_dest_size(toplevel.frame_buffer, frame_size.width(), frame_size.height());
    use_nearest_filter(&toplevel.frame_buffer->node);
    wlr_buffer_drop(buffer);

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
static void apply_toplevel_decoration(Toplevel& toplevel)
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
    apply_toplevel_decoration(*toplevel);
    toplevel_update_frame(*toplevel);
    // The new window joins the current workspace stack.
    WindowManager::on_toplevel_mapped(*toplevel);
    // Apply a restored session position (R4.2), then report the final position.
    SessionManager::on_toplevel_mapped(*toplevel);
    // [Placement] window rules: set the new window's geometry (overrides the
    // session restore), so a script can reproduce a specific desktop layout.
    if (toplevel->xdg_toplevel->title) {
        if (auto placement = Config::window_placement_for(StringView { toplevel->xdg_toplevel->title, strlen(toplevel->xdg_toplevel->title) }); placement.has_value()) {
            auto* geometry = &toplevel->xdg_toplevel->base->geometry;
            wlr_scene_node_set_position(&toplevel->scene_tree->node, placement->x() - geometry->x, placement->y() - geometry->y);
            wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, placement->width(), placement->height());
            toplevel->restore_position = placement->location();
        }
    }
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
    // A decoration created (and possibly requested) before the surface was
    // initialized is applied now that it is.
    apply_toplevel_decoration(*toplevel);
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
        // SerenityOS: dragging a maximized window restores its size, keeping the
        // cursor at the same horizontal fraction of the window.
        if (toplevel.maximized && !toplevel.restore_size.is_empty()) {
            int spanned_width = max(1, toplevel.frame_size.width());
            double fraction_x = clamp((server.cursor->x - toplevel.scene_tree->node.x) / spanned_width, 0.0, 1.0);
            toplevel.maximized = false;
            wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, false);
            wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, toplevel.restore_size.width(), toplevel.restore_size.height());
            if (toplevel.foreign_handle)
                wlr_foreign_toplevel_handle_v1_set_maximized(toplevel.foreign_handle, false);
            double new_x = server.cursor->x - fraction_x * toplevel.restore_size.width();
            wlr_scene_node_set_position(&toplevel.scene_tree->node, new_x, toplevel.scene_tree->node.y);
            toplevel_report_position(toplevel);
        }
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

// --- window animations ----------------------------------------------------
//
// SerenityOS animates minimize/restore as an inverted outline that interpolates
// between the window's frame and its Taskbar button (Window::start_minimize_
// animation). We draw the same outline (a ChromeNode in the overlay tree) over
// 150 ms, gated on [Effects] AnimateWindows, using the button rect the Taskbar
// reports over the WindowManagerServer (set_window_taskbar_rect).
struct MinimizeAnimation {
    Gfx::IntRect from;
    Gfx::IntRect to;
    i64 start_ms { 0 };
    OwnPtr<ChromeNode> node;
};
static Vector<MinimizeAnimation> s_minimize_animations;
static RefPtr<Core::Timer> s_animation_timer;
static constexpr int minimize_animation_duration_ms = 150;

static Core::ElapsedTimer& animation_clock()
{
    static Core::ElapsedTimer clock = Core::ElapsedTimer::start_new();
    return clock;
}

static Gfx::IntRect interpolate_rect(Gfx::IntRect const& a, Gfx::IntRect const& b, float t)
{
    auto lerp = [t](int from, int to) {
        return from + static_cast<int>(static_cast<float>(to - from) * t + (t >= 0.5f ? 0.5f : -0.5f));
    };
    return { lerp(a.x(), b.x()), lerp(a.y(), b.y()), lerp(a.width(), b.width()), lerp(a.height(), b.height()) };
}

static void tick_minimize_animations()
{
    i64 const now = animation_clock().elapsed_milliseconds();
    bool any_active = false;
    for (auto& animation : s_minimize_animations) {
        float t = clamp(static_cast<float>(now - animation.start_ms) / minimize_animation_duration_ms, 0.0f, 1.0f);
        float eased = 1.0f - (1.0f - t) * (1.0f - t); // ease-out
        auto rect = interpolate_rect(animation.from, animation.to, eased);
        if (g_palette && animation.node && rect.width() > 0 && rect.height() > 0) {
            auto color = g_palette->selection();
            animation.node->set_visible(true);
            animation.node->set_rect(rect);
            animation.node->render([&](Gfx::Painter& painter) {
                painter.draw_rect_with_thickness({ 0, 0, rect.width(), rect.height() }, color, 2);
            });
        }
        if (t < 1.0f)
            any_active = true;
    }
    if (!any_active) {
        s_minimize_animations.clear(); // destroys the ChromeNodes (hides them)
        if (s_animation_timer)
            s_animation_timer->stop();
    }
}

static void start_minimize_animation(Server& server, Toplevel& toplevel, Gfx::IntRect const& window_rect, bool minimizing)
{
    if (!server.overlay_tree || !Config::effects().animate_windows || window_rect.is_empty())
        return;

    Gfx::IntRect target = toplevel.taskbar_rect;
    if (target.is_empty()) {
        // No Taskbar button (no Taskbar, or one that has not reported yet): use
        // the bottom-right corner (the applet/clock end of the Taskbar).
        auto* output_state = output_for(server, server.primary_output);
        Gfx::IntRect area { 0, 0, 1280, 720 };
        if (output_state && !wlr_box_empty(&output_state->usable_area))
            area = { output_state->usable_area.x, output_state->usable_area.y, output_state->usable_area.width, output_state->usable_area.height };
        target = { area.right() - 24, area.bottom() - 24, 4, 4 };
    }

    MinimizeAnimation animation;
    animation.from = minimizing ? window_rect : target;
    animation.to = minimizing ? target : window_rect;
    animation.start_ms = animation_clock().elapsed_milliseconds();
    animation.node = make<ChromeNode>(*server.overlay_tree);
    animation.node->set_rect(animation.from);
    s_minimize_animations.append(move(animation));
    dbgln("WindowServer: {} animation {}x{}+{},{} -> {}x{}+{},{}",
        minimizing ? "minimize" : "restore",
        animation.from.width(), animation.from.height(), animation.from.x(), animation.from.y(),
        animation.to.width(), animation.to.height(), animation.to.x(), animation.to.y());

    if (!s_animation_timer)
        s_animation_timer = Core::Timer::create_repeating(15, tick_minimize_animations);
    s_animation_timer->start();
}

// Minimize/restore a window: state, visibility, the foreign handle, and the
// outline animation. Shared by the compositor's own minimize and the Taskbar's.
static void set_toplevel_minimized(Server& server, Toplevel& toplevel, bool minimized)
{
    if (toplevel.minimized == minimized)
        return;

    Gfx::IntRect window_rect {
        toplevel.scene_tree->node.x, toplevel.scene_tree->node.y,
        toplevel.frame_size.width(), toplevel.frame_size.height()
    };
    if (window_rect.is_empty())
        window_rect = { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y, toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };

    toplevel.minimized = minimized;
    WindowManager::update_toplevel_visibility(toplevel);
    if (toplevel.foreign_handle)
        wlr_foreign_toplevel_handle_v1_set_minimized(toplevel.foreign_handle, minimized);
    start_minimize_animation(server, toplevel, window_rect, minimized);
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
    set_toplevel_minimized(*toplevel.server, toplevel, !toplevel.minimized);
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
    // Every window has an icon (SerenityOS behaviour): the client's, else the
    // default, so the titlebar, switcher and Taskbar always have one to draw.
    toplevel->icon = g_default_window_icon;
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

// Tell Xwayland the usable area(s) (_NET_WORKAREA), so X11 clients (panels,
// maximized windows, fullscreen) respect SDE's panels/docks. docs/20 item 8.
static void update_xwayland_workareas(Server& server)
{
    if (!server.xwayland || !server.xwayland_started)
        return;
    Vector<wlr_box> areas;
    for (auto& output : server.outputs) {
        wlr_box box = output->usable_area;
        if (wlr_box_empty(&box))
            wlr_output_layout_get_box(server.output_layout, output->output, &box);
        areas.append(box);
    }
    if (areas.is_empty())
        return;
    wlr_xwayland_set_workareas(server.xwayland, areas.data(), areas.size());
    dbgln("WindowServer: reported X11 workarea ({} area(s); first {}x{} at {},{}).",
        areas.size(), areas[0].width, areas[0].height, areas[0].x, areas[0].y);
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

    // Keep X11 clients' workarea in step with the panels' exclusive zones.
    update_xwayland_workareas(server);
}

// On_demand layer surfaces (client popups: the Wi-Fi network list and its
// passphrase field, ComboBox dropdowns, …) take keyboard focus while they are
// mapped, so their text fields can be typed into. Panels, the desktop and
// applets use KEYBOARD_INTERACTIVITY_NONE and never steal focus.
static bool layer_takes_keyboard(LayerSurface* layer)
{
    return layer->layer_surface->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
}

static void focus_layer_surface(Server& server, LayerSurface* layer)
{
    auto* surface = layer->layer_surface->surface;
    if (!surface || server.seat->keyboard_state.focused_surface == surface)
        return;
    stop_key_repeat();
    server.keyboard_focus_layer = layer;
    auto* keyboard = wlr_seat_get_keyboard(server.seat);
    if (keyboard)
        wlr_seat_keyboard_notify_enter(server.seat, surface, keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
}

// Give keyboard focus back to the topmost normal window (or clear it) when an
// on_demand layer surface stops being focused.
static void restore_keyboard_focus(Server& server)
{
    if (server.keyboard_focus_layer)
        server.keyboard_focus_layer = nullptr;
    stop_key_repeat();
    for (auto it = server.toplevels.rbegin(); it != server.toplevels.rend(); ++it) {
        if (!(*it)->minimized) {
            focus_toplevel(server, it->ptr());
            return;
        }
    }
    wlr_seat_keyboard_notify_clear_focus(server.seat);
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
    uint32_t const committed_width = layer->layer_surface->surface ? layer->layer_surface->surface->current.width : 0;
    uint32_t const committed_height = layer->layer_surface->surface ? layer->layer_surface->surface->current.height : 0;
    bool const arrange_relevant = !previous.valid
        || previous.desired_width != state.desired_width
        || previous.desired_height != state.desired_height
        || previous.width != committed_width
        || previous.height != committed_height
        || previous.anchor != state.anchor
        || previous.exclusive_zone != state.exclusive_zone
        || previous.margin_top != state.margin.top
        || previous.margin_right != state.margin.right
        || previous.margin_bottom != state.margin.bottom
        || previous.margin_left != state.margin.left
        || previous.layer != static_cast<uint32_t>(state.layer);
    // Keyboard focus follows on_demand interactivity (a client may toggle it).
    if (layer->layer_surface->surface && layer->layer_surface->surface->mapped) {
        if (layer_takes_keyboard(layer))
            focus_layer_surface(*layer->server, layer);
        else if (layer->server->keyboard_focus_layer == layer)
            restore_keyboard_focus(*layer->server);
    }
    if (!arrange_relevant)
        return;

    layer->arrange_state.valid = true;
    layer->arrange_state.desired_width = state.desired_width;
    layer->arrange_state.desired_height = state.desired_height;
    layer->arrange_state.width = committed_width;
    layer->arrange_state.height = committed_height;
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
    // A mapped on_demand surface (a client popup) takes keyboard focus, so its
    // text fields work.
    if (layer_takes_keyboard(layer))
        focus_layer_surface(*layer->server, layer);
}

static void layer_surface_unmap(wl_listener* listener, void*)
{
    LayerSurface* layer = nullptr;
    layer = wl_container_of(listener, layer, unmap);
    wlr_scene_node_set_enabled(&layer->scene->tree->node, false);
    bool const had_focus = layer->server->keyboard_focus_layer == layer;
    arrange_layers(*layer->server, layer->output);
    if (had_focus)
        restore_keyboard_focus(*layer->server);
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
    bool const had_focus = server->keyboard_focus_layer == layer;
    remove_owned(server->layer_surfaces, layer);
    arrange_layers(*server, output);
    if (had_focus)
        restore_keyboard_focus(*server);
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
    if (!decoration->toplevel)
        return;
    // Honour the client's request (CLIENT_SIDE -> no compositor frame; anything
    // else -> server-side decorations). Applied now if initialized, else at the
    // initial commit.
    apply_toplevel_decoration(*decoration->toplevel);
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

    // Honour the client's decoration choice (Firefox/GTK ask for CSD); apply it
    // now if the toplevel is initialized, else at the initial commit.
    if (decoration->toplevel) {
        decoration->toplevel->decoration = wlr_decoration;
        apply_toplevel_decoration(*decoration->toplevel);
    }
    server->decorations.append(move(decoration));
}

// --- output management ----------------------------------------------------

// R4.5: persist the live output layout to screenlayout.ini so it survives a
// restart and is visible to DisplaySettings (via the port's get_screen_layout).
// True if the live layout already matches what screenlayout.ini holds (matched by
// device), so persisting would be a no-op. Skipping avoids pointless writes that
// race with a live edit, re-trigger the outputs watcher, and (during startup) could
// clobber the saved entries.
static bool live_layout_matches_loaded(Vector<Config::OutputSettings> const& computed)
{
    auto const& loaded = Config::outputs();
    if (computed.size() != loaded.size())
        return false;
    for (auto const& entry : computed) {
        bool found = false;
        for (auto const& existing : loaded) {
            if (existing.device == entry.device && existing.mode == entry.mode
                && existing.left == entry.left && existing.top == entry.top
                && existing.width == entry.width && existing.height == entry.height
                && existing.scale == entry.scale) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

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
        // The SerenityOS screenlayout.ini ScaleFactor is an integer; a fractional
        // scale is recorded as 0 ("unspecified") so a reload re-decides it from
        // [Output] DefaultScale / the auto rule instead of a wrong integer.
        entry.scale = output_scale_is_integral() ? static_cast<int>(output.output->scale) : 0;
        settings.append(move(entry));
        if (output.output == server.primary_output)
            main_index = index;
    }
    if (live_layout_matches_loaded(settings))
        return;
    Config::set_outputs(move(settings), main_index);
}

// Persist the layout, coalesced. Outputs are created in a burst at startup (and a
// config edit can re-scale several at once); writing after each would clobber the
// saved entries for outputs not yet present, and the outputs-file watcher would
// then reload the truncated file and place the remaining outputs with add_auto
// (the multi-output position bug). One short timer writes the settled layout once.
static void schedule_persist_screen_layout(Server& server)
{
    static RefPtr<Core::Timer> timer;
    static Server* target = nullptr;
    target = &server;
    timer = Core::Timer::create_single_shot(120, [] {
        if (target)
            persist_screen_layout(*target);
    });
    timer->start();
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
    if (mode)
        wlr_output_state_set_mode(&state, mode);
    else if (settings.width > 0 && settings.height > 0)
        wlr_output_state_set_custom_mode(&state, settings.width, settings.height, 0);
    else if (auto* preferred = wlr_output_preferred_mode(output))
        wlr_output_state_set_mode(&state, preferred);
    // The resolved scale honours SDE_OUTPUT_SCALE / [Output] DefaultScale first, so
    // a configured scale wins over the persisted (integer) layout.
    float const scale = resolve_output_scale(output, mode, &settings);
    wlr_output_state_set_scale(&state, scale);
    wlr_output_commit_state(output, &state);
    wlr_output_state_finish(&state);

    wlr_output_layout_add(server.output_layout, output, settings.left, settings.top);
    dbgln("WindowServer: applied screen layout to '{}': {}x{} @{}x at {},{}",
        output->name ? output->name : "?", settings.width, settings.height, scale, settings.left, settings.top);
}

// R6 HiDPI: re-apply the resolved output scale to the live outputs. Called at
// startup and whenever [Output] DefaultScale changes so a config edit takes effect
// immediately (Live reload, docs/14 §4).
static void apply_output_scale(Server& server)
{
    bool changed = false;
    for (auto& output : server.outputs) {
        Config::OutputSettings const* saved = nullptr;
        for (auto const& settings : Config::outputs()) {
            if (output->output->name && settings.mode == "Device"sv && settings.device == output->output->name) {
                saved = &settings;
                break;
            }
        }
        float const scale = resolve_output_scale(output->output, wlr_output_preferred_mode(output->output), saved);
        if (output->output->scale == scale)
            continue;

        wlr_output_state state;
        wlr_output_state_init(&state);
        wlr_output_state_set_enabled(&state, true);
        // Keep the current mode; only the scale changes.
        wlr_output_state_set_scale(&state, scale);
        if (wlr_output_commit_state(output->output, &state)) {
            set_output_scale(output->output->scale);
            changed = true;
            dbgln("WindowServer: output '{}' re-scaled to {}x", output->output->name ? output->output->name : "?", output->output->scale);
            update_desktop_background(server, *output);
            arrange_layers(server, output->output);
        }
        wlr_output_state_finish(&state);
    }
    if (changed) {
        output_manager_send_configuration(server);
        schedule_persist_screen_layout(server);
    }
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
        schedule_persist_screen_layout(server);
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

// set_icon: the client supplies its window icon as memory-mappable pixels
// (SerenityOS Window::icon()). wl_shm ARGB8888 is little-endian B,G,R,A, which
// matches Gfx::BitmapFormat::BGRA8888.
static void serenity_toplevel_set_icon(wl_client*, wl_resource* resource, int32_t width, int32_t height, int32_t stride, uint32_t format, int32_t fd)
{
    auto* chrome = static_cast<SerenityChrome*>(wl_resource_get_user_data(resource));
    Toplevel* toplevel = chrome ? chrome->toplevel : nullptr;
    if (!toplevel) {
        if (fd >= 0)
            close(fd);
        return;
    }

    // width 0 (or an unusable descriptor) clears the icon.
    if (width <= 0 || height <= 0 || stride <= 0 || fd < 0) {
        if (fd >= 0)
            close(fd);
        toplevel->icon = g_default_window_icon;
        WindowManager::emit_window_icon(*toplevel);
        toplevel_update_frame(*toplevel);
        return;
    }
    if (format != 0) { // WL_SHM_FORMAT_ARGB8888
        warnln("WindowServer: unsupported window icon format {}", format);
        close(fd);
        return;
    }

    size_t const size = static_cast<size_t>(stride) * static_cast<size_t>(height);
    void* mapping = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapping != MAP_FAILED) {
        if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { width, height }); !bitmap.is_error()) {
            auto& created = *bitmap.value();
            for (int y = 0; y < height; ++y)
                memcpy(created.scanline(y), static_cast<u8 const*>(mapping) + static_cast<size_t>(y) * stride, min(static_cast<size_t>(stride), static_cast<size_t>(created.pitch())));
            toplevel->icon = bitmap.release_value();
            dbgln("WindowServer: composed window icon {}x{} for wm_id={}", width, height, toplevel->wm_id);
        }
        munmap(mapping, size);
    }
    close(fd);
    WindowManager::emit_window_icon(*toplevel);
    toplevel_update_frame(*toplevel);
}

static struct serenity_toplevel_interface const s_serenity_toplevel_implementation = {
    .set_icon = serenity_toplevel_set_icon,
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

// --- serenity-virtual-input (test harness) ---------------------------------
// The headless harness has no libinput and wlroots offers no virtual touch or
// tablet, so the compositor exposes a private injection protocol; requests feed
// the same routing as real input (see handle_touch_* / handle_tablet_*).
static void virtual_touch_down(wl_client*, wl_resource* resource, uint32_t time, int32_t touch_id, wl_fixed_t x, wl_fixed_t y)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_down(server, time, touch_id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_touch_motion(wl_client*, wl_resource* resource, uint32_t time, int32_t touch_id, wl_fixed_t x, wl_fixed_t y)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_motion(server, time, touch_id, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_touch_up(wl_client*, wl_resource* resource, uint32_t time, int32_t touch_id)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_up(server, time, touch_id);
}

static void virtual_touch_cancel(wl_client*, wl_resource* resource, uint32_t)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    handle_touch_cancel(server);
}

static void virtual_touch_destroy_request(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static struct serenity_virtual_touch_v1_interface const virtual_touch_implementation = {
    .down = virtual_touch_down,
    .motion = virtual_touch_motion,
    .up = virtual_touch_up,
    .cancel = virtual_touch_cancel,
    .destroy = virtual_touch_destroy_request,
};

static void virtual_touch_resource_destroy(wl_resource* resource)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    if (server.virtual_touch_count > 0)
        --server.virtual_touch_count;
    update_seat_capabilities(server);
}

static void virtual_input_manager_create_touch(wl_client* client, wl_resource* resource, uint32_t id)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    auto* touch = wl_resource_create(client, &serenity_virtual_touch_v1_interface, 1, id);
    if (!touch) {
        wl_client_post_no_memory(client);
        return;
    }
    // Advertise the touch capability while a virtual touch device exists, so
    // clients can bind wl_touch and receive the injected events.
    ++server.virtual_touch_count;
    update_seat_capabilities(server);
    wl_resource_set_implementation(touch, &virtual_touch_implementation, &server, virtual_touch_resource_destroy);
}

// A hand-built tablet device + pen tool. wlr_tablet_from_input_device() is a
// cast, so the v2 wrappers accept these; kept alive with the resource.
struct VirtualTabletDevice {
    Server* server { nullptr };
    wlr_tablet tablet { };
    wlr_tablet_tool tool { };
};

static void virtual_tablet_proximity_in(wl_client*, wl_resource* resource, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_proximity(*device->server, &device->tablet, &device->tool, true, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_tablet_proximity_out(wl_client*, wl_resource* resource, uint32_t)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_proximity(*device->server, &device->tablet, &device->tool, false, 0, 0);
}

static void virtual_tablet_tip_down(wl_client*, wl_resource* resource, uint32_t)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_tip(*device->server, &device->tool, true);
}

static void virtual_tablet_tip_up(wl_client*, wl_resource* resource, uint32_t)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_tip(*device->server, &device->tool, false);
}

static void virtual_tablet_motion(wl_client*, wl_resource* resource, uint32_t, wl_fixed_t x, wl_fixed_t y)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_motion(*device->server, &device->tool, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void virtual_tablet_axis(wl_client*, wl_resource* resource, uint32_t, wl_fixed_t pressure, wl_fixed_t distance, wl_fixed_t tilt_x, wl_fixed_t tilt_y, wl_fixed_t rotation, wl_fixed_t slider, wl_fixed_t wheel)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_axis(*device->server, &device->tool, wl_fixed_to_double(pressure), wl_fixed_to_double(distance),
        wl_fixed_to_double(tilt_x), wl_fixed_to_double(tilt_y), wl_fixed_to_double(rotation), wl_fixed_to_double(slider), wl_fixed_to_double(wheel));
}

static void virtual_tablet_button(wl_client*, wl_resource* resource, uint32_t, uint32_t button, uint32_t pressed)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    handle_tablet_button(*device->server, &device->tool, button, pressed != 0);
}

static void virtual_tablet_destroy_request(wl_client*, wl_resource* resource)
{
    wl_resource_destroy(resource);
}

static struct serenity_virtual_tablet_v1_interface const virtual_tablet_implementation = {
    .proximity_in = virtual_tablet_proximity_in,
    .proximity_out = virtual_tablet_proximity_out,
    .tip_down = virtual_tablet_tip_down,
    .tip_up = virtual_tablet_tip_up,
    .motion = virtual_tablet_motion,
    .axis = virtual_tablet_axis,
    .button = virtual_tablet_button,
    .destroy = virtual_tablet_destroy_request,
};

static void virtual_tablet_resource_destroy(wl_resource* resource)
{
    auto* device = static_cast<VirtualTabletDevice*>(wl_resource_get_user_data(resource));
    if (!device)
        return;
    auto& server = *device->server;
    // Tear down the v2 wrappers (they listen on these synthetic signals), then
    // drop the map entries and free.
    if (server.tablet_tool_map.contains(reinterpret_cast<u64>(&device->tool))) {
        wl_signal_emit_mutable(&device->tool.events.destroy, &device->tool);
        server.tablet_tool_map.remove(reinterpret_cast<u64>(&device->tool));
    }
    if (server.tablet_map.contains(reinterpret_cast<u64>(&device->tablet))) {
        wl_signal_emit_mutable(&device->tablet.base.events.destroy, &device->tablet.base);
        server.tablet_map.remove(reinterpret_cast<u64>(&device->tablet));
    }
    free(device->tablet.base.name);
    delete device;
}

static void virtual_input_manager_create_tablet(wl_client* client, wl_resource* resource, uint32_t id)
{
    auto& server = *static_cast<Server*>(wl_resource_get_user_data(resource));
    auto* device = new VirtualTabletDevice();
    device->server = &server;
    device->tablet.base.type = WLR_INPUT_DEVICE_TABLET;
    device->tablet.base.name = strdup("sde-virtual-tablet");
    wl_signal_init(&device->tablet.base.events.destroy);
    device->tablet.width_mm = 100;
    device->tablet.height_mm = 100;
    device->tool.type = WLR_TABLET_TOOL_TYPE_PEN;
    wl_signal_init(&device->tool.events.destroy);

    auto* tablet = wl_resource_create(client, &serenity_virtual_tablet_v1_interface, 1, id);
    if (!tablet) {
        delete device;
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(tablet, &virtual_tablet_implementation, device, virtual_tablet_resource_destroy);
}

static struct serenity_virtual_input_manager_v1_interface const virtual_input_manager_implementation = {
    .create_touch = virtual_input_manager_create_touch,
    .create_tablet = virtual_input_manager_create_tablet,
};

static void virtual_input_manager_bind(wl_client* client, void* data, uint32_t version, uint32_t id)
{
    auto& server = *static_cast<Server*>(data);
    auto* resource = wl_resource_create(client, &serenity_virtual_input_manager_v1_interface, min(version, 1u), id);
    if (!resource) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(resource, &virtual_input_manager_implementation, &server, nullptr);
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
        stop_key_repeat();
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
    stop_key_repeat();
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
    stop_key_repeat();
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

// --- R5 protocol parity: xdg-toplevel-icon-v1 + xdg-system-bell-v1 (docs/21) --

static Toplevel* toplevel_for_xdg(Server& server, wlr_xdg_toplevel* xdg_toplevel)
{
    for (auto& toplevel : server.toplevels) {
        if (toplevel->xdg_toplevel == xdg_toplevel)
            return toplevel.ptr();
    }
    return nullptr;
}

// wl_shm fourcc channel order: ARGB8888/XRGB8888 are B,G,R,(X) in memory; the
// XBGR/ABGR variants are R,G,B,(X). (Same values as the DRM formats.)
static bool shm_format_is_bgr(uint32_t format)
{
    return format == 0x34325241u /* ARGB8888 */ || format == 0x34325258u /* XRGB8888 */;
}

static bool shm_format_is_rgb(uint32_t format)
{
    return format == 0x34324241u /* ABGR8888 */ || format == 0x34324258u /* XBGR8888 */;
}

// xdg-toplevel-icon-v1: a client set (or cleared) a window icon. Convert the
// chosen buffer to a Gfx::Bitmap and report it, like the X11 _NET_WM_ICON path
// (docs/20 item 3) — the Taskbar button and the Alt-Tab switcher draw it.
static void toplevel_icon_set(wl_listener* listener, void* data)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, toplevel_icon_set);
    auto* event = static_cast<wlr_xdg_toplevel_icon_manager_v1_set_icon_event*>(data);
    auto* toplevel = toplevel_for_xdg(*server, event->toplevel);
    if (!toplevel)
        return;

    RefPtr<Gfx::Bitmap> icon;
    if (event->icon) {
        // Prefer a scale-1 buffer nearest 16x16 (the Taskbar/switcher size).
        wlr_xdg_toplevel_icon_v1_buffer* best = nullptr;
        int best_score = -1000000;
        wlr_xdg_toplevel_icon_v1_buffer* buffer = nullptr;
        wl_list_for_each(buffer, &event->icon->buffers, link)
        {
            if (!buffer->buffer)
                continue;
            int const w = buffer->buffer->width;
            int const distance = w >= 16 ? w - 16 : 16 - w;
            int const score = (buffer->scale == 1 ? 100 : 0) - distance;
            if (score > best_score) {
                best_score = score;
                best = buffer;
            }
        }
        if (best) {
            auto* buffer = best->buffer;
            void* pixels = nullptr;
            uint32_t format = 0;
            size_t stride = 0;
            if (wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &pixels, &format, &stride)) {
                int const w = buffer->width;
                int const h = buffer->height;
                if ((shm_format_is_bgr(format) || shm_format_is_rgb(format)) && w > 0 && h > 0 && stride >= static_cast<size_t>(w) * 4) {
                    if (auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { w, h }); !bitmap.is_error()) {
                        auto& created = *bitmap.value();
                        for (int y = 0; y < h; ++y) {
                            auto const* src = static_cast<uint8_t const*>(pixels) + static_cast<size_t>(y) * stride;
                            auto* dst = created.scanline(y);
                            if (shm_format_is_bgr(format)) {
                                memcpy(dst, src, static_cast<size_t>(w) * 4);
                            } else {
                                for (int x = 0; x < w; ++x) {
                                    dst[x * 4 + 0] = src[x * 4 + 2];
                                    dst[x * 4 + 1] = src[x * 4 + 1];
                                    dst[x * 4 + 2] = src[x * 4 + 0];
                                    dst[x * 4 + 3] = src[x * 4 + 3];
                                }
                            }
                        }
                        icon = bitmap.release_value();
                    }
                }
                wlr_buffer_end_data_ptr_access(buffer);
            }
        }
    }

    toplevel->icon = icon ? move(icon) : g_default_window_icon;
    WindowManager::emit_window_icon(*toplevel);
    toplevel_update_frame(*toplevel);
    dbgln("WindowServer: xdg-toplevel-icon set for wm_id={} ({}x{})", toplevel->wm_id,
        toplevel->icon ? toplevel->icon->width() : 0, toplevel->icon ? toplevel->icon->height() : 0);
}

// xdg-system-bell-v1: a client asked for the system bell. Advertise the global so
// such clients do not fail; the audible/visual bell itself is not implemented.
static void system_bell_ring(wl_listener* listener, void*)
{
    Server* server = nullptr;
    server = wl_container_of(listener, server, system_bell_ring);
    (void)server;
    dbgln("WindowServer: xdg-system-bell ring (bell output not implemented)");
}

static ErrorOr<void> initialize(Server& server)
{
    server.display = wl_display_create();
    if (!server.display)
        return Error::from_string_literal("failed to create wl_display");

    // R4.3/R6: seat identity. The session exports XDG_SEAT/XDG_VTNR (from the
    // display manager or logind); log them so multi-seat sessions are identifiable.
    if (auto const* seat = getenv("XDG_SEAT"); seat && *seat) {
        if (auto const* vt = getenv("XDG_VTNR"); vt && *vt)
            dbgln("WindowServer: seat '{}' vt{}", seat, vt);
        else
            dbgln("WindowServer: seat '{}'", seat);
    }

    server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), nullptr);
    if (!server.backend) {
        // R4.3: the DRM/libinput backend needs a seat (libseat). Give actionable
        // guidance rather than a bare failure so a TTY login is recoverable.
        warnln("WindowServer: could not create a backend (DRM/libinput needs a seat).");
        warnln("  * install and start seatd (recommended), or run under systemd-logind;");
        warnln("  * for direct TTY access (builtin), join the 'video', 'render' and 'input' groups and re-login;");
        warnln("  * or use a nested/headless session (scripts/start-sde-nested.sh).");
        warnln("  * SDE_SEAT_BACKEND/LIBSEAT_BACKEND/SEATD_SOCK select the seat backend.");
        return Error::from_string_literal("failed to create wlr_backend");
    }

    // R6/N5 renderer selection: [Output] Renderer in WindowServer.ini, with the
    // SDE_RENDERER environment variable overriding it (the config-test pattern).
    // It maps onto wlroots' `WLR_RENDERER`.
    ByteString requested_renderer_storage;
    if (auto const* renderer = getenv("SDE_RENDERER"); renderer && *renderer)
        requested_renderer_storage = renderer;
    else
        requested_renderer_storage = Config::output().renderer;
    char const* requested_renderer = requested_renderer_storage.is_empty() ? "auto" : requested_renderer_storage.characters();
    if (!requested_renderer_storage.is_empty())
        setenv("WLR_RENDERER", requested_renderer, true);

    bool used_software_fallback = false;
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        // The GPU path failed (no DRM render node, EGL bring-up failure,
        // unsupported driver, ...). Rather than ending the session, retry with
        // wlroots' software (pixman) renderer, which needs no device. (LibGPU/
        // LibSoftGPU are the *client* GL software path; they cannot back a
        // wlr_renderer.)
        wlr_log(WLR_ERROR, "failed to create a GPU renderer; falling back to the software (pixman) renderer");
        setenv("WLR_RENDERER", "pixman", true);
        server.renderer = wlr_renderer_autocreate(server.backend);
        used_software_fallback = true;
    }
    if (!server.renderer)
        return Error::from_string_literal("failed to create wlr_renderer");
    if (used_software_fallback)
        dbgln("WindowServer: renderer 'pixman' (software fallback from '{}')", requested_renderer);
    else
        dbgln("WindowServer: renderer '{}'", requested_renderer);
    wlr_renderer_init_wl_display(server.renderer, server.display);

    server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
    if (!server.allocator)
        return Error::from_string_literal("failed to create wlr_allocator");

    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
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
    // Drag icons render above every layer (R5 explicit drag-and-drop).
    server.drag_icon_tree = wlr_scene_tree_create(&server.scene->tree);
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
    // Test harness: virtual touch/tablet injection (see serenity-virtual-input).
    wl_global_create(server.display, &serenity_virtual_input_manager_v1_interface, 1, &server, virtual_input_manager_bind);
    SessionManager::initialize(server);
    // R5: InputCapture portal support (compositor-hosted EIS; docs/17).
    InputCapture::initialize(server);

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

    // R5 protocol parity (docs/21-r5-protocol-parity-plan.md): client window icons
    // and the system bell, both available on the pinned wlroots 0.20.
    server.toplevel_icon_manager = wlr_xdg_toplevel_icon_manager_v1_create(server.display, 1);
    if (server.toplevel_icon_manager) {
        static int sizes[] = { 16, 32, 64 };
        wlr_xdg_toplevel_icon_manager_v1_set_sizes(server.toplevel_icon_manager, sizes, 3);
        server.toplevel_icon_set.notify = toplevel_icon_set;
        wl_signal_add(&server.toplevel_icon_manager->events.set_icon, &server.toplevel_icon_set);
    }
    server.system_bell = wlr_xdg_system_bell_v1_create(server.display, 1);
    if (server.system_bell) {
        server.system_bell_ring.notify = system_bell_ring;
        wl_signal_add(&server.system_bell->events.ring, &server.system_bell_ring);
    }
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
    server.touch_down.notify = cursor_touch_down;
    wl_signal_add(&server.cursor->events.touch_down, &server.touch_down);
    server.touch_motion.notify = cursor_touch_motion;
    wl_signal_add(&server.cursor->events.touch_motion, &server.touch_motion);
    server.touch_up.notify = cursor_touch_up;
    wl_signal_add(&server.cursor->events.touch_up, &server.touch_up);
    server.touch_cancel.notify = cursor_touch_cancel;
    wl_signal_add(&server.cursor->events.touch_cancel, &server.touch_cancel);
    server.tablet_tool_axis.notify = tablet_tool_axis;
    wl_signal_add(&server.cursor->events.tablet_tool_axis, &server.tablet_tool_axis);
    server.tablet_tool_proximity.notify = tablet_tool_proximity;
    wl_signal_add(&server.cursor->events.tablet_tool_proximity, &server.tablet_tool_proximity);
    server.tablet_tool_tip.notify = tablet_tool_tip;
    wl_signal_add(&server.cursor->events.tablet_tool_tip, &server.tablet_tool_tip);
    server.tablet_tool_button.notify = tablet_tool_button;
    wl_signal_add(&server.cursor->events.tablet_tool_button, &server.tablet_tool_button);

    server.new_input.notify = server_new_input;
    wl_signal_add(&server.backend->events.new_input, &server.new_input);

    server.seat = wlr_seat_create(server.display, "seat0");

    // XWayland (R5): X11 client support. [Compositor] XWayland=false disables it
    // entirely (no server, no DISPLAY). Lazy: the X server starts on first use.
    if (Config::compositor().xwayland && server.compositor) {
        server.xwayland = wlr_xwayland_create(server.display, server.compositor, true);
        if (server.xwayland) {
            wlr_xwayland_set_seat(server.xwayland, server.seat);
            server.new_xwayland_surface.notify = server_new_xwayland_surface;
            wl_signal_add(&server.xwayland->events.new_surface, &server.new_xwayland_surface);
            server.xwayland_ready.notify = xwayland_ready;
            wl_signal_add(&server.xwayland->events.ready, &server.xwayland_ready);
            // Export DISPLAY for the session's apps (state file + our own env).
            setenv("DISPLAY", server.xwayland->display_name, 1);
            StringView display { server.xwayland->display_name, strlen(server.xwayland->display_name) };
            Config::write_state("xwayland-display"sv, display.bytes());
            dbgln("WindowServer: Xwayland enabled (DISPLAY={})", server.xwayland->display_name);
        } else {
            warnln("WindowServer: could not create Xwayland");
        }
    }
    server.seat_request_cursor.notify = seat_request_cursor;
    wl_signal_add(&server.seat->events.request_set_cursor, &server.seat_request_cursor);
    server.seat_pointer_focus_change.notify = seat_pointer_focus_change;
    wl_signal_add(&server.seat->pointer_state.events.focus_change, &server.seat_pointer_focus_change);
    server.seat_request_set_selection.notify = seat_request_set_selection;
    wl_signal_add(&server.seat->events.request_set_selection, &server.seat_request_set_selection);
    server.seat_request_set_primary_selection.notify = seat_request_set_primary_selection;
    wl_signal_add(&server.seat->events.request_set_primary_selection, &server.seat_request_set_primary_selection);
    // R5: explicit drag-and-drop (data-device DnD between windows).
    server.seat_request_start_drag.notify = seat_request_start_drag;
    wl_signal_add(&server.seat->events.request_start_drag, &server.seat_request_start_drag);
    server.seat_start_drag.notify = seat_start_drag;
    wl_signal_add(&server.seat->events.start_drag, &server.seat_start_drag);

    // R5: the native-app compatibility surface. Most of these protocols are fully
    // implemented by wlroots, so the compositor only advertises them; relative
    // pointer and keyboard-shortcuts-inhibit also need a little glue (below and
    // in keyboard_key()).
    wlr_primary_selection_v1_device_manager_create(server.display);
    // data-control lets clipboard managers (e.g. the SDE ClipboardHistory applet)
    // observe and set the selection and primary selection. wlroots implements the
    // protocol; both the wlr and the ext variants are advertised.
    wlr_data_control_manager_v1_create(server.display);
    wlr_ext_data_control_manager_v1_create(server.display, 1);
    wlr_viewporter_create(server.display);
    wlr_content_type_manager_v1_create(server.display, 1);
    wlr_tearing_control_manager_v1_create(server.display, 1);
    // R6: more standard protocols wlroots implements; advertising them is all
    // that is needed (solid-colour buffers, per-surface opacity, modal dialogs).
    wlr_single_pixel_buffer_manager_v1_create(server.display);
    wlr_alpha_modifier_v1_create(server.display);
    wlr_xdg_wm_dialog_v1_create(server.display, 1);
    // R6 presentation-time: frame feedback (presented events). Fed from
    // output_frame() via wlr_presentation_surface_textured_on_output().
    wlr_presentation_create(server.display, server.backend, 2);
    // R6 HiDPI: fractional output scale. wlr_scene notifies each mapped surface of
    // the preferred (possibly fractional) scale, so native clients render at it.
    wlr_fractional_scale_manager_v1_create(server.display, 1);
    server.relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server.display);
    server.pointer_gestures = wlr_pointer_gestures_v1_create(server.display);
    server.tablet_manager = wlr_tablet_v2_create(server.display);
    server.keyboard_shortcuts_inhibit = wlr_keyboard_shortcuts_inhibit_v1_create(server.display);
    server.pointer_constraints = wlr_pointer_constraints_v1_create(server.display);
    server.new_pointer_constraint.notify = new_pointer_constraint;
    wl_signal_add(&server.pointer_constraints->events.new_constraint, &server.new_pointer_constraint);
    auto* foreign_registry = wlr_xdg_foreign_registry_create(server.display);
    wlr_xdg_foreign_v1_create(server.display, foreign_registry);
    wlr_xdg_foreign_v2_create(server.display, foreign_registry);

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
    g_resource_root = resource_root;
    load_overlay_shadow();

    g_frame_icons.close = load_icon(resource_root, "window-close"sv, "window-close"sv);
    g_frame_icons.minimize = load_icon(resource_root, "window-minimize"sv, "downward-triangle"sv);
    g_frame_icons.maximize = load_icon(resource_root, "window-maximize"sv, "upward-triangle"sv);
    g_frame_icons.restore = load_icon(resource_root, "window-restore"sv, "upward-triangle"sv);
    g_frame_icons.move = load_icon(resource_root, "move"sv, ""sv);
    g_frame_icons.pin = load_icon(resource_root, "window-pin"sv, ""sv);
    g_default_window_icon = load_icon(resource_root, "window"sv, "window"sv);
    g_x11_default_icon = make_x11_default_icon(g_palette ? g_palette->active_window_title() : Gfx::Color::White);

    // Wallpaper/background: load the persisted state, seeding from
    // SDE_WALLPAPER / SDE_WALLPAPER_MODE on the first run.
    initialize_wallpaper(resource_root);

    // The compositor's own configuration (WindowServer.ini) lives beside the
    // wallpaper/theme state; load it now and reload on change.
    Config::load();
    // R6/N8: apply [Log] Level unless SDE_LOG_LEVEL already overrode it.
    if (!getenv("SDE_LOG_LEVEL") && !Config::log().level.is_empty()) {
        if (auto level = log_level_from_string(Config::log().level.view()); level.has_value())
            apply_log_level(*level, Config::log().level.view());
    }
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
    load_overlay_shadow();
    for (auto& toplevel : server.toplevels)
        toplevel->frame_cache.clear();
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
    // The titlebar text is drawn from the title font, so a font change must
    // re-render frames the cache would otherwise consider unchanged.
    for (auto& toplevel : server.toplevels)
        toplevel->frame_cache.clear();
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

// R6/N8 crash-dump policy. A fatal signal appends a short report (including a
// backtrace) to $SDE_CRASH_LOG (default: the state dir's WindowServer-crash.log)
// and then re-raises, so the supervisor restarts the session and we still have a
// diagnostic. The handler only uses async-signal-safe calls (write/backtrace_
// symbols_fd) and never swallows the crash.
static ByteString s_crash_log_path;
static int s_crash_log_fd { -1 };

static void crash_write(char const* data, size_t length)
{
    while (length > 0) {
        auto written = ::write(s_crash_log_fd, data, length);
        if (written <= 0)
            return;
        data += written;
        length -= static_cast<size_t>(written);
    }
}

static void fatal_signal_handler(int signal_number)
{
    char const* name = "signal";
    switch (signal_number) {
    case SIGSEGV: name = "SIGSEGV"; break;
    case SIGABRT: name = "SIGABRT"; break;
    case SIGBUS: name = "SIGBUS"; break;
    case SIGILL: name = "SIGILL"; break;
    case SIGFPE: name = "SIGFPE"; break;
    default: break;
    }
    // open(2) is async-signal-safe, so the log file is created lazily (a normal
    // exit never leaves an empty crash log behind).
    s_crash_log_fd = ::open(s_crash_log_path.characters(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s_crash_log_fd >= 0) {
        char const* header = "\nWindowServer: crashed with ";
        crash_write(header, strlen(header));
        crash_write(name, strlen(name));
        char const* suffix = " (backtrace follows)\n";
        crash_write(suffix, strlen(suffix));
        void* frames[64];
        int frame_count = backtrace(frames, 64);
        backtrace_symbols_fd(frames, frame_count, s_crash_log_fd);
        ::close(s_crash_log_fd);
        s_crash_log_fd = -1;
    }
    // Restore the default action and re-raise, so the process still dies with the
    // signal (the supervisor sees a non-zero exit and restarts).
    signal(signal_number, SIG_DFL);
    raise(signal_number);
}

static void install_crash_report_handler()
{
    if (auto const* configured = getenv("SDE_CRASH_LOG"); configured && *configured)
        s_crash_log_path = configured;
    else
        s_crash_log_path = ByteString::formatted("{}/WindowServer-crash.log", Config::state_directory());
    if (auto directory = LexicalPath(s_crash_log_path).dirname(); !directory.is_empty())
        (void)Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes);
    struct sigaction action { };
    action.sa_handler = fatal_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    for (int signal_number : { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE })
        sigaction(signal_number, &action, nullptr);
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    wlr_log_init(WLR_ERROR, nullptr);
    install_crash_report_handler();
    // R6/N8: SDE_LOG_LEVEL overrides [Log] Level (which is applied once the
    // config loads).
    if (auto const* env_level = getenv("SDE_LOG_LEVEL"); env_level && *env_level) {
        StringView env_level_view { env_level, strlen(env_level) };
        if (auto level = log_level_from_string(env_level_view); level.has_value())
            apply_log_level(*level, env_level_view);
    }

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
            outln("Renderer={}", Config::output().renderer.is_empty() ? "auto"sv : Config::output().renderer.view());
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
            outln("[Log]");
            outln("Level={}", Config::log().level.is_empty() ? "error"sv : Config::log().level.view());
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

    // Publish the configured xkb layout to clients in the standard
    // XKB_DEFAULT_* variables, so a client that wants the active layout (e.g. the
    // Keymap applet, through LibKeyboard) can read it. The keymap itself still
    // travels to clients over wl_keyboard.
    auto const& keyboard = Config::input().keyboard;
    if (!keyboard.layouts.is_empty())
        setenv("XKB_DEFAULT_LAYOUT", keyboard.layouts.characters(), true);
    if (!keyboard.variant.is_empty())
        setenv("XKB_DEFAULT_VARIANT", keyboard.variant.characters(), true);

    // The WindowServer serves the WindowManagerServer protocol (the shell's
    // Taskbar protocol), just like SerenityOS's WindowServer does. The socket
    // path can be overridden so tests can run isolated from a live session.
    ByteString wm_portal { "/tmp/portal/wm" };
    if (auto const* override_path = getenv("SDE_WM_PORTAL_PATH"); override_path && *override_path)
        wm_portal = override_path;
    // Ensure the parent directory exists: /tmp/portal is not guaranteed to survive a
    // tmp-clean, and an override may name a fresh directory. (Previously this was
    // only done for the default path, so a session that exported SDE_WM_PORTAL_PATH
    // =/tmp/portal/wm skipped it and listen() failed - leaving a login with applets
    // but no Taskbar/desktop, as the WM clients could not connect.)
    if (auto directory = LexicalPath(wm_portal).dirname(); !directory.is_empty())
        (void)Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes);
    ::unlink(wm_portal.characters());
    auto wm_server = Core::LocalServer::construct();
    if (!wm_server->listen(wm_portal))
        warnln("WindowServer: could not listen on {}", wm_portal);
    // Let the window manager call back into the compositor's focus/menu code.
    WindowManager::set_host_hooks({ focus_toplevel, open_window_menu, primary_usable_area, toplevel_report_position, toplevel_begin_interactive, current_palette, update_ext_workspace_active, set_toplevel_minimized, hook_focus_x11, hook_set_minimized_x11 });
    WindowSwitcher::set_host_hooks({ current_palette, focus_toplevel, WindowManager::switch_workspace, overlay_shadow_hook, toplevel_update_frame, hook_focus_x11, hook_set_minimized_x11, x11_update_frame });
    AppletManager::set_host_hooks({ WindowManager::tell_wms_applet_area_size_changed, primary_usable_area, layer_report_position });
    // Bring the workspace grid up from the config, and follow later edits.
    WindowManager::load_workspaces(*server);
    Config::watch([&] {
        WindowManager::load_workspaces(*server);
        // R6 HiDPI: a live [Output] DefaultScale edit re-scales the outputs.
        apply_output_scale(*server);
        // A live [Applet] Order edit re-lays out the applet area.
        AppletManager::relayout(*server);
    });
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

    // Start the shell only now that the WindowManagerServer is listening and wired:
    // the Taskbar, WorkspacePicker and Keymap connect to /tmp/portal/wm at startup
    // and abort if the connect fails, so spawning them before listen() races on a
    // real login (the applets that don't use the WM connection were unaffected).
    spawn_startup_command(startup_command);

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

    // R6/N4: a liveness heartbeat for the session supervisor's watchdog. A wedged
    // event loop stops updating the file, so the supervisor can restart the
    // session instead of hanging forever. Off unless SDE_HEARTBEAT_FILE is set
    // (the session supervisor sets it), so nested/headless runs are unaffected.
    if (auto const* heartbeat = getenv("SDE_HEARTBEAT_FILE"); heartbeat && *heartbeat) {
        ByteString heartbeat_path { heartbeat };
        auto beat = [heartbeat_path] {
            auto file = Core::File::open(heartbeat_path, Core::File::OpenMode::Write);
            if (file.is_error())
                return;
            auto text = ByteString::number(static_cast<long>(::time(nullptr)));
            (void)file.value()->write_until_depleted(text.bytes());
        };
        beat();
        static RefPtr<Core::Timer> heartbeat_timer;
        heartbeat_timer = Core::Timer::create_repeating(1000, [beat] { beat(); });
        heartbeat_timer->start();
    }

    int result = loop.exec();
    wl_display_destroy_clients(server->display);
    wl_display_destroy(server->display);
    return result;
}
