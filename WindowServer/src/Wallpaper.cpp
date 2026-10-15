/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Output.h"
#include "Wallpaper.h"

#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/Timer.h>
#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "Wlroots.h"

// Desktop background: a wallpaper image (optional) drawn over the theme's
// background colour. The Display Settings app writes the choice (image, mode and
// colour) to a shared state directory; we load it at startup and watch it.
RefPtr<Gfx::Bitmap> g_wallpaper;

ByteString g_wallpaper_mode { "center" };

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

void write_sde_wallpaper(RefPtr<Gfx::Bitmap> const& bitmap)
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

void update_desktop_background(Server& server, Output& output)
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
void load_wallpaper_state()
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
void initialize_wallpaper(ByteString const& resource_root)
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

void watch_wallpaper_state(Server& server)
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
