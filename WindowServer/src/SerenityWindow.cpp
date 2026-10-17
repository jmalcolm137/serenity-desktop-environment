/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Frame.h"
#include "Resources.h"
#include "SerenityWindow.h"
#include "Capture.h"
#include "Wallpaper.h"
#include "WindowManager.h"
#include "XdgShell.h"

#include <AK/StringBuilder.h>
#include <LibCore/ConfigFile.h>
#include <WindowServer/WindowMode.h>

#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "Wlroots.h"

extern "C" {
#include "serenity-window-server-protocol.h"
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

// set_window_mode: the client's SerenityOS WindowMode (0 modeless, 1 passive,
// 2 render-above, 3 blocking). A blocking window is a modal dialog: it preempts
// its modal chain, so input behind it is blocked and it cannot be minimized
// (SerenityOS Window::blocking_modal_window()).
static void serenity_toplevel_set_window_mode(wl_client*, wl_resource* resource, uint32_t mode)
{
    auto* chrome = static_cast<SerenityChrome*>(wl_resource_get_user_data(resource));
    Toplevel* toplevel = chrome ? chrome->toplevel : nullptr;
    if (!toplevel)
        return;

    toplevel->modal = mode == static_cast<uint32_t>(WindowServer::WindowMode::Blocking);
    // The transient parent is the xdg toplevel's parent (set by the client).
    toplevel->transient_parent = nullptr;
    if (toplevel->xdg_toplevel && toplevel->xdg_toplevel->parent) {
        for (auto& other : toplevel->server->toplevels) {
            if (other->xdg_toplevel == toplevel->xdg_toplevel->parent) {
                toplevel->transient_parent = other.ptr();
                break;
            }
        }
    }
    dbgln("WindowServer: toplevel wm_id={} window mode {} (modal={}, transient={})",
        toplevel->wm_id, mode, toplevel->modal, toplevel->transient_parent ? toplevel->transient_parent->wm_id : 0);
    // A dialog sits above its parent.
    if (toplevel->transient_parent)
        wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
    // A modal blocks (or unblocks) the other windows: re-report them all.
    for (auto& other : toplevel->server->toplevels)
        WindowManager::emit_window_state(*other);
}

// set_launch_origin: the rect the launcher handed the application (SerenityOS's
// __libgui_launch_origin_rect). The compositor animates the window out of it when
// the toplevel maps (Window::start_launch_animation).
static void serenity_toplevel_set_launch_origin(wl_client*, wl_resource* resource, int32_t x, int32_t y, int32_t width, int32_t height)
{
    auto* chrome = static_cast<SerenityChrome*>(wl_resource_get_user_data(resource));
    Toplevel* toplevel = chrome ? chrome->toplevel : nullptr;
    if (!toplevel)
        return;
    if (width <= 0 || height <= 0) {
        toplevel->launch_origin = {};
        return;
    }
    toplevel->launch_origin = { x, y, width, height };
}

static struct serenity_toplevel_interface const s_serenity_toplevel_implementation = {
    .set_icon = serenity_toplevel_set_icon,
    .destroy = serenity_toplevel_request_destroy,
    .set_window_mode = serenity_toplevel_set_window_mode,
    .set_launch_origin = serenity_toplevel_set_launch_origin,
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
    forget_capture_requests(resource);
    if (auto* server = static_cast<Server*>(wl_resource_get_user_data(resource)))
        server->serenity_window_managers.remove_first_matching([&](auto* candidate) { return candidate == resource; });
}

// Where a semantic setting lives in the desktop's configuration. The desktop
// owns the layout, so this table is here, not in the application framework.
struct HostSettingLocation {
    char const* file;
    char const* section;
    char const* entry;
};

static Optional<HostSettingLocation> host_setting_location(StringView key)
{
    static auto* table = [] {
        auto* map = new HashMap<ByteString, HostSettingLocation>;
        auto add = [&](StringView name, char const* file, char const* section, char const* entry) {
            map->set(ByteString { name }, HostSettingLocation { file, section, entry });
        };
        add("mouse.acceleration"sv, "input.ini", "Mouse", "Acceleration");
        add("mouse.scroll_step_size"sv, "input.ini", "Mouse", "ScrollStepSize");
        add("mouse.double_click_speed"sv, "input.ini", "Mouse", "DoubleClickSpeed");
        add("mouse.buttons_switched"sv, "input.ini", "Mouse", "ButtonsSwitched");
        add("mouse.natural_scroll"sv, "input.ini", "Mouse", "NaturalScroll");
        add("mouse.tap_to_click"sv, "input.ini", "Mouse", "TapToClick");
        add("mouse.left_handed"sv, "input.ini", "Mouse", "LeftHanded");
        add("mouse.accel_profile"sv, "input.ini", "Mouse", "AccelProfile");
        add("keyboard.layouts"sv, "input.ini", "Keyboard", "Layouts");
        add("keyboard.variant"sv, "input.ini", "Keyboard", "Variant");
        add("keyboard.model"sv, "input.ini", "Keyboard", "Model");
        add("keyboard.options"sv, "input.ini", "Keyboard", "Options");
        add("cursor.theme"sv, "cursor.ini", "Cursor", "Theme");
        add("cursor.highlight_radius"sv, "cursor.ini", "Highlight", "Radius");
        add("cursor.highlight_color"sv, "cursor.ini", "Highlight", "Color");
        add("workspaces.rows"sv, "WindowServer.ini", "Workspaces", "Rows");
        add("workspaces.columns"sv, "WindowServer.ini", "Workspaces", "Columns");
        add("theme.name"sv, "WindowServer.ini", "Theme", "Name");
        add("theme.path"sv, "WindowServer.ini", "Theme", "Path");
        add("effects.show_geometry"sv, "WindowServer.ini", "Effects", "ShowGeometry");
        add("effects.tile_window"sv, "WindowServer.ini", "Effects", "TileWindow");
        add("output.default_scale"sv, "WindowServer.ini", "Output", "DefaultScale");
        return map;
    }();
    if (auto it = table->find(ByteString { key }); it != table->end())
        return it->value;
    // The per-effect flags are dynamic: effects.<SystemEffects name>.
    if (key.starts_with("effects."sv))
        return HostSettingLocation { "WindowServer.ini", "Effects", nullptr };
    return {};
}

// The settings the shim answers from, as key=value lines (see the protocol).
static ByteString host_settings_blob()
{
    StringBuilder builder;
    auto add = [&](StringView key, StringView value) {
        builder.appendff("{}={}\n", key, value);
    };
    auto const& input = Config::input();
    add("mouse.acceleration"sv, ByteString::formatted("{}", input.mouse.acceleration));
    add("mouse.scroll_step_size"sv, ByteString::number(input.mouse.scroll_step_size));
    add("mouse.double_click_speed"sv, ByteString::number(input.mouse.double_click_speed));
    add("mouse.buttons_switched"sv, input.mouse.buttons_switched ? "true"sv : "false"sv);
    add("mouse.natural_scroll"sv, input.mouse.natural_scroll ? "true"sv : "false"sv);
    add("mouse.tap_to_click"sv, input.mouse.tap_to_click ? "true"sv : "false"sv);
    add("mouse.left_handed"sv, input.mouse.left_handed ? "true"sv : "false"sv);
    add("mouse.accel_profile"sv, input.mouse.accel_profile);
    add("keyboard.layouts"sv, input.keyboard.layouts);
    add("keyboard.variant"sv, input.keyboard.variant);
    add("keyboard.model"sv, input.keyboard.model);
    add("keyboard.options"sv, input.keyboard.options);
    add("theme.name"sv, Config::theme().name);
    add("theme.path"sv, Config::theme().path);
    add("effects.show_geometry"sv, Config::effects().show_geometry);
    add("effects.tile_window"sv, Config::effects().tile_window);
    add("workspaces.rows"sv, ByteString::number(Config::workspaces().rows));
    add("workspaces.columns"sv, ByteString::number(Config::workspaces().columns));
    if (auto cursor = Core::ConfigFile::open(ByteString::formatted("{}/cursor.ini", Config::state_directory())); !cursor.is_error()) {
        add("cursor.theme"sv, cursor.value()->read_entry("Cursor"sv, "Theme"sv, "Default"sv));
        add("cursor.highlight_radius"sv, ByteString::number(cursor.value()->read_num_entry("Highlight"sv, "Radius"sv, 0)));
        add("cursor.highlight_color"sv, cursor.value()->read_entry("Highlight"sv, "Color"sv, "#000000"sv));
    }
    // The output layout (R4.5): "main=<i>;<Mode>,<Device>,<Left>,<Top>,<W>,<H>,<Scale>;...".
    StringBuilder layout;
    layout.appendff("main={}", Config::main_screen_index());
    for (auto const& output : Config::outputs())
        layout.appendff(";{},{},{},{},{},{},{}", output.mode, output.device, output.left, output.top, output.width, output.height, output.scale);
    add("screenlayout"sv, layout.string_view());
    add("wallpaper.mode"sv, g_wallpaper_mode);
    return builder.to_byte_string();
}

void broadcast_host_settings(Server& server)
{
    auto blob = host_settings_blob();
    for (auto* manager : server.serenity_window_managers) {
        if (manager)
            serenity_window_manager_send_host_settings(manager, blob.characters());
    }
}

// set_host_setting: the application changed a setting the desktop owns. Apply and
// persist it in our own configuration (the shim does not touch our files).
// set_wallpaper: the application set a wallpaper. The desktop owns it: store the
// pixels and show them (the shim does not touch our state).
static void serenity_window_manager_set_wallpaper(wl_client*, wl_resource* resource, int32_t width, int32_t height, int32_t pitch, uint32_t format, int32_t fd)
{
    auto* server = static_cast<Server*>(wl_resource_get_user_data(resource));
    if (fd < 0 || width <= 0 || height <= 0 || pitch <= 0) {
        // "No wallpaper" (a zero size, or a client without one). A client may
        // still have handed us a placeholder fd, so close it.
        if (fd >= 0)
            ::close(fd);
        g_wallpaper = nullptr;
        write_sde_wallpaper(nullptr);
    } else {
        auto size = static_cast<size_t>(pitch) * static_cast<size_t>(height);
        auto* mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (mapped != MAP_FAILED) {
            if (auto bitmap = Gfx::Bitmap::create(static_cast<Gfx::BitmapFormat>(format), { width, height }); !bitmap.is_error()) {
                auto& created = *bitmap.value();
                auto const row_bytes = min(static_cast<size_t>(pitch), static_cast<size_t>(created.pitch()));
                for (int y = 0; y < height; ++y)
                    memcpy(created.scanline(y), static_cast<u8 const*>(mapped) + static_cast<size_t>(y) * static_cast<size_t>(pitch), row_bytes);
                g_wallpaper = bitmap.release_value();
                write_sde_wallpaper(g_wallpaper);
            }
            munmap(mapped, size);
        }
        ::close(fd);
    }
    load_wallpaper_state();
    dbgln("WindowServer: wallpaper set by a client ({}x{})", width, height);
    if (server)
        broadcast_host_settings(*server);
}

static void serenity_window_manager_set_host_setting(wl_client*, wl_resource* resource, char const* key, char const* value)
{
    auto* server = static_cast<Server*>(wl_resource_get_user_data(resource));
    // State-file settings and the output layout are not ini entries.
    if (StringView { key, strlen(key) } == "screenlayout"sv) {
        Vector<Config::OutputSettings> outputs;
        unsigned main_screen = 0;
        for (auto field : StringView { value, strlen(value) }.split_view(';')) {
            if (field.starts_with("main="sv)) {
                main_screen = field.substring_view(5).to_number<unsigned>().value_or(0);
                continue;
            }
            auto parts = field.split_view(',');
            if (parts.size() < 7)
                continue;
            Config::OutputSettings output;
            output.mode = parts[0];
            output.device = parts[1];
            output.left = parts[2].to_number<int>().value_or(0);
            output.top = parts[3].to_number<int>().value_or(0);
            output.width = parts[4].to_number<int>().value_or(0);
            output.height = parts[5].to_number<int>().value_or(0);
            output.scale = parts[6].to_number<int>().value_or(1);
            outputs.append(move(output));
        }
        Config::set_outputs(move(outputs), main_screen);
        dbgln("WindowServer: screen layout set by a client ({} screens)", Config::outputs().size());
        if (server)
            broadcast_host_settings(*server);
        return;
    }
    if (StringView { key, strlen(key) } == "wallpaper.mode"sv) {
        Config::write_state("wallpaper.mode"sv, StringView { value, strlen(value) }.bytes());
        load_wallpaper_state();
        if (server)
            broadcast_host_settings(*server);
        return;
    }
    if (StringView { key, strlen(key) } == "wallpaper.color"sv) {
        Config::write_state("wallpaper.color"sv, StringView { value, strlen(value) }.bytes());
        load_wallpaper_state();
        if (server)
            broadcast_host_settings(*server);
        return;
    }

    auto location = host_setting_location(StringView { key, strlen(key) });
    if (!location.has_value()) {
        dbgln("WindowServer: unknown host setting '{}'", key);
        return;
    }
    ByteString const entry = location->entry ? ByteString { location->entry }
        : ByteString { StringView { key, strlen(key) }.substring_view(strlen("effects.")) };
    ByteString path = ByteString::formatted("{}/{}", Config::state_directory(), location->file);
    auto config = Core::ConfigFile::open(path, Core::ConfigFile::AllowWriting::Yes);
    if (config.is_error()) {
        dbgln("WindowServer: could not open '{}' for host setting '{}'", path, key);
        return;
    }
    config.value()->write_entry(ByteString { location->section }, entry, ByteString { value, strlen(value) });
    (void)config.value()->sync();
    dbgln("WindowServer: host setting {}={} ({} [{}/{}])", key, value, location->file, location->section, entry);
    // Our config watcher reloads; tell every shim so its answers stay current.
    if (server)
        broadcast_host_settings(*server);
}

// R5 accessibility (docs/29): one capture of a screen region for the Magnifier.
// Pushed back as `captured`; see the protocol description for why this is a push.
static void serenity_window_manager_request_capture(wl_client*, wl_resource* resource, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height)
{
    if (!wl_resource_get_user_data(resource))
        return;
    queue_capture_request(resource, serial, { x, y, width, height });
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
        .set_host_setting = serenity_window_manager_set_host_setting,
        .set_wallpaper = serenity_window_manager_set_wallpaper,
        .get_toplevel = serenity_window_manager_get_toplevel,
        .request_capture = serenity_window_manager_request_capture,
    };
    wl_resource_set_implementation(resource, &implementation, &server, serenity_window_manager_resource_destroy);
    server.serenity_window_managers.append(resource);

    // The desktop owns the window-server configuration an application asks for
    // (the workspace grid and the input-setting bounds); hand it to the shim so it
    // answers those requests without reading our config file. See docs/24.
    serenity_window_manager_send_host_settings(resource, host_settings_blob().characters());
    // Only when there is one: a client with no wallpaper keeps none.
    if (g_wallpaper) {
        int wallpaper = bitmap_fd(g_wallpaper);
        if (wallpaper >= 0) {
            serenity_window_manager_send_wallpaper(resource, g_wallpaper->width(), g_wallpaper->height(),
                g_wallpaper->pitch(), static_cast<uint32_t>(g_wallpaper->format()), wallpaper);
            ::close(wallpaper);
        }
    }
    serenity_window_manager_send_host_config(resource,
        static_cast<uint32_t>(Config::workspaces().rows), static_cast<uint32_t>(Config::workspaces().columns),
        wl_fixed_from_double(Config::input_ranges().accel_min), wl_fixed_from_double(Config::input_ranges().accel_max),
        Config::input_ranges().scroll_step_min, Config::input_ranges().double_click_min, Config::input_ranges().double_click_max);
}

void initialize_serenity_window(Server& server)
{
    wl_global_create(server.display, &serenity_window_manager_interface, 1, &server, serenity_window_manager_bind);
}
