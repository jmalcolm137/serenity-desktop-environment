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

    // Write the header and rows directly. A header+pixels ByteBuffer would be a
    // second full copy of the image, which is pure peak heap for a write that the
    // kernel can consume row by row.
    auto file = Core::File::open(Config::state_file("wallpaper.raw"sv), Core::File::OpenMode::Write);
    if (file.is_error())
        return;
    auto& output = *file.value();
    WallpaperHeader header {
        wallpaper_magic,
        static_cast<u32>(bitmap->format()),
        static_cast<u32>(bitmap->width()),
        static_cast<u32>(bitmap->height()),
        static_cast<u32>(bitmap->pitch()),
    };
    if (output.write_until_depleted({ &header, sizeof(header) }).is_error())
        return;
    auto const row_bytes = static_cast<size_t>(bitmap->pitch());
    for (int y = 0; y < bitmap->height(); ++y) {
        if (output.write_until_depleted({ bitmap->scanline(y), row_bytes }).is_error())
            return;
    }
}

static RefPtr<Gfx::Bitmap> read_sde_wallpaper()
{
    auto file = Core::File::open(Config::state_file("wallpaper.raw"sv), Core::File::OpenMode::Read);
    if (file.is_error())
        return nullptr;
    auto& input = *file.value();

    WallpaperHeader header {};
    if (input.read_until_filled({ &header, sizeof(header) }).is_error())
        return nullptr;
    if (header.magic != wallpaper_magic || header.width == 0 || header.height == 0 || header.pitch == 0
        || header.format == 0 || !Gfx::is_valid_bitmap_format(header.format))
        return nullptr;
    auto bitmap = Gfx::Bitmap::create(static_cast<Gfx::BitmapFormat>(header.format), { static_cast<int>(header.width), static_cast<int>(header.height) });
    if (bitmap.is_error())
        return nullptr;
    auto& created = *bitmap.value();
    auto const row_bytes = min(static_cast<size_t>(header.pitch), static_cast<size_t>(created.pitch()));
    for (int y = 0; y < created.height(); ++y) {
        if (input.read_until_filled({ created.scanline(y), row_bytes }).is_error())
            return nullptr;
        auto const extra = static_cast<i64>(header.pitch) - static_cast<i64>(row_bytes);
        if (extra > 0 && input.seek(extra, SeekMode::FromCurrentPosition).is_error())
            return nullptr;
    }
    return bitmap.release_value();
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

static void clear_wallpaper_nodes(DesktopBackground& background)
{
    if (background.wallpaper) {
        wlr_scene_node_destroy(&background.wallpaper->node);
        background.wallpaper = nullptr;
    }
    background.wallpaper_nodes.clear();
}

static void add_wallpaper_node(DesktopBackground& background, wlr_buffer* buffer, Gfx::IntRect dest, Optional<wlr_fbox> source)
{
    auto* node = wlr_scene_buffer_create(background.wallpaper, buffer);
    wlr_scene_node_set_position(&node->node, dest.x(), dest.y());
    wlr_scene_buffer_set_dest_size(node, dest.width(), dest.height());
    if (source.has_value())
        wlr_scene_buffer_set_source_box(node, &source.value());
    // The old CPU compositor used nearest-neighbour scaling for the wallpaper.
    wlr_scene_buffer_set_filter_mode(node, WLR_SCALE_FILTER_NEAREST);
    background.wallpaper_nodes.append(node);
}

static void dump_background(Gfx::IntSize size, Gfx::Color background_color, StringView dump_path)
{
    auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, size);
    if (bitmap.is_error())
        return;
    Gfx::Painter painter(*bitmap.value());
    painter.fill_rect(bitmap.value()->rect(), background_color);
    if (g_wallpaper) {
        auto const& wallpaper = *g_wallpaper;
        if (g_wallpaper_mode == "tile"sv) {
            painter.draw_tiled_bitmap(bitmap.value()->rect(), wallpaper);
        } else if (g_wallpaper_mode == "stretch"sv) {
            painter.draw_scaled_bitmap(bitmap.value()->rect(), wallpaper, wallpaper.rect());
        } else if (g_wallpaper_mode == "fill"sv && wallpaper.width() > 0 && wallpaper.height() > 0) {
            double const scale = max(static_cast<double>(size.width()) / wallpaper.width(), static_cast<double>(size.height()) / wallpaper.height());
            int const src_width = min(wallpaper.width(), static_cast<int>(size.width() / scale));
            int const src_height = min(wallpaper.height(), static_cast<int>(size.height() / scale));
            Gfx::IntRect const src { (wallpaper.width() - src_width) / 2, (wallpaper.height() - src_height) / 2, src_width, src_height };
            painter.draw_scaled_bitmap(bitmap.value()->rect(), wallpaper, src);
        } else {
            Gfx::IntPoint offset { (size.width() - wallpaper.width()) / 2, (size.height() - wallpaper.height()) / 2 };
            painter.blit(offset, wallpaper, wallpaper.rect());
        }
    }
    if (auto encoded = Gfx::PNGWriter::encode(*bitmap.value()); !encoded.is_error()) {
        if (auto file = Core::File::open(dump_path, Core::File::OpenMode::Write); !file.is_error())
            (void)file.value()->write_until_depleted(encoded.value());
    }
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

    Gfx::IntSize const size { width, height };
    auto const background_color = g_background_color.value_or(g_palette->desktop_background());
    auto& background = output.background;

    if (background.tree) {
        // Layout coordinates can change independently of the output size: keep the
        // background at the output's current origin even on the early-out path.
        wlr_box box { };
        wlr_output_layout_get_box(server.output_layout, output.output, &box);
        wlr_scene_node_set_position(&background.tree->node, box.x, box.y);
    }

    if (background.tree
        && background.output_size == size
        && background.wallpaper_bitmap == g_wallpaper.ptr()
        && background.wallpaper_mode == g_wallpaper_mode
        && background.color_value == background_color)
        return;

    if (auto const* dump_path = getenv("WINDOWSERVER_DUMP_BACKGROUND"); dump_path && *dump_path)
        dump_background(size, background_color, StringView { dump_path, strlen(dump_path) });

    if (!background.tree) {
        background.tree = wlr_scene_tree_create(&server.scene->tree);
        wlr_scene_node_lower_to_bottom(&background.tree->node);
    }
    if (!background.color_rect) {
        float const transparent[4] = { 0, 0, 0, 0 };
        background.color_rect = wlr_scene_rect_create(background.tree, width, height, transparent);
    }
    wlr_scene_rect_set_size(background.color_rect, width, height);
    set_scene_rect_color(background.color_rect, background_color);
    wlr_scene_node_set_position(&background.color_rect->node, 0, 0);

    clear_wallpaper_nodes(background);

    if (g_wallpaper && g_wallpaper_mode != "none"sv) {
        background.wallpaper = wlr_scene_tree_create(background.tree);

        // A tiny tile (the 8x4 grid) would otherwise create tens of thousands of
        // scene nodes. Build one reasonably sized repeated pattern and tile that.
        RefPtr<Gfx::Bitmap> pattern;
        if (g_wallpaper_mode == "tile"sv) {
            int pattern_width = g_wallpaper->width();
            int pattern_height = g_wallpaper->height();
            if (pattern_width > 0 && pattern_height > 0 && (pattern_width < 256 || pattern_height < 256)) {
                int const repeats_x = max(1, (256 + pattern_width - 1) / pattern_width);
                int const repeats_y = max(1, (256 + pattern_height - 1) / pattern_height);
                pattern_width *= repeats_x;
                pattern_height *= repeats_y;
                auto bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, { pattern_width, pattern_height });
                if (!bitmap.is_error()) {
                    Gfx::Painter painter(*bitmap.value());
                    painter.clear_rect(bitmap.value()->rect(), Gfx::Color::Transparent);
                    painter.draw_tiled_bitmap(bitmap.value()->rect(), *g_wallpaper);
                    pattern = bitmap.release_value();
                }
            }
        }

        auto wallpaper_bitmap = pattern ? pattern : g_wallpaper;
        auto wallpaper_ref = wallpaper_bitmap;
        auto* buffer = bitmap_buffer_create(wallpaper_ref.release_nonnull());
        auto const wallpaper_width = wallpaper_bitmap->width();
        auto const wallpaper_height = wallpaper_bitmap->height();

        if (g_wallpaper_mode == "tile"sv && wallpaper_width > 0 && wallpaper_height > 0) {
            for (int y = 0; y < height; y += wallpaper_height) {
                for (int x = 0; x < width; x += wallpaper_width)
                    add_wallpaper_node(background, buffer, { x, y, wallpaper_width, wallpaper_height }, OptionalNone {});
            }
        } else if (g_wallpaper_mode == "stretch"sv) {
            add_wallpaper_node(background, buffer, { 0, 0, width, height }, OptionalNone {});
        } else if (g_wallpaper_mode == "fill"sv && wallpaper_width > 0 && wallpaper_height > 0) {
            double const scale = max(static_cast<double>(width) / wallpaper_width, static_cast<double>(height) / wallpaper_height);
            int const src_width = min(wallpaper_width, static_cast<int>(width / scale));
            int const src_height = min(wallpaper_height, static_cast<int>(height / scale));
            wlr_fbox source { };
            source.x = (wallpaper_width - src_width) / 2.0f;
            source.y = (wallpaper_height - src_height) / 2.0f;
            source.width = src_width;
            source.height = src_height;
            add_wallpaper_node(background, buffer, { 0, 0, width, height }, source);
        } else {
            // Center (the SerenityOS default).
            int const x = (width - wallpaper_width) / 2;
            int const y = (height - wallpaper_height) / 2;
            add_wallpaper_node(background, buffer, { x, y, wallpaper_width, wallpaper_height }, OptionalNone {});
        }
        wlr_buffer_drop(buffer);
    }

    background.output_size = size;
    background.wallpaper_bitmap = g_wallpaper.ptr();
    background.wallpaper_mode = g_wallpaper_mode;
    background.color_value = background_color;
}

// Re-read only the mode and colour. A compositor-originated set_wallpaper already
// has the pixels in g_wallpaper; re-reading the file would allocate another full
// copy of the image for no change.
void load_wallpaper_mode_and_color()
{
    g_wallpaper_mode = "center";
    if (auto mode = Config::read_state("wallpaper.mode"sv); !mode.is_empty())
        g_wallpaper_mode = mode.to_lowercase();

    g_background_color = {};
    if (auto color = Config::read_state("wallpaper.color"sv); !color.is_empty())
        g_background_color = Gfx::Color::from_string(color);
}

// Re-read the persisted wallpaper state into the globals (no repaint yet).
void load_wallpaper_state()
{
    g_wallpaper = read_sde_wallpaper();
    load_wallpaper_mode_and_color();
}

// Repaint every output from the current g_wallpaper/mode/colour.
void apply_wallpaper_to_outputs(Server& server)
{
    for (auto& output : server.outputs) {
        output->background.output_size = { };
        update_desktop_background(server, *output);
    }
}

static bool s_ignore_next_wallpaper_state_event { false };

void ignore_next_wallpaper_state_event()
{
    s_ignore_next_wallpaper_state_event = true;
}

// Re-read the state and repaint every output (used on a live change).
static void apply_wallpaper_from_state(Server& server)
{
    // A compositor-originated set_wallpaper applies the new image immediately and
    // writes the state file; the watcher's event for that write must not reload
    // and rebuild the same pixels.
    if (s_ignore_next_wallpaper_state_event) {
        s_ignore_next_wallpaper_state_event = false;
        return;
    }
    load_wallpaper_state();
    apply_wallpaper_to_outputs(server);
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
