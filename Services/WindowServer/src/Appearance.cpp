/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Compositor.h"
#include "Appearance.h"
#include "CursorHighlight.h"
#include "Frame.h"
#include "Resources.h"
#include "SerenityWindow.h"
#include "Wallpaper.h"

#include <AK/LexicalPath.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/File.h>
#include <LibCore/FileWatcher.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Palette.h>
#include <LibGfx/SystemTheme.h>

#include <fcntl.h>
#include <stdlib.h>

#include "Wlroots.h"

static ByteString theme_broker_path()
{
    if (auto const* runtime = getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return ByteString::formatted("{}/sde-theme", runtime);
    return ByteString { "/tmp/sde-theme" };
}

void apply_theme_path(Server& server, ByteString const& theme_path)
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
        output->background.output_size = { };
        update_desktop_background(server, *output);
    }
    dbgln("WindowServer: applied system theme '{}'", theme_path);
}

// R5 accessibility (docs/29-accessibility.md): the palette the compositor forces
// when [Accessibility] HighContrast is on.
ByteString high_contrast_theme_path()
{
    return ByteString::formatted("{}/themes/Contrast.ini", g_resource_root);
}

// Publish a theme to the broker: that is how a live theme choice reaches every
// client (Display Settings writes it when the user picks one).
void publish_theme_to_broker(ByteString const& theme_path)
{
    auto file = Core::File::open(theme_broker_path(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file.is_error()) {
        warnln("WindowServer: could not open the theme broker to publish '{}': {}", theme_path, file.error());
        return;
    }
    if (auto written = file.value()->write_until_depleted(theme_path.bytes()); written.is_error())
        warnln("WindowServer: could not publish the theme to the broker: {}", written.error());
}

void watch_theme_broker(Server& server)
{
    static RefPtr<Core::FileWatcher> watcher;
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto path = theme_broker_path();
    watcher->on_change = [&server, path](Core::FileWatcherEvent const& event) {
        if (event.event_path != path)
            return;
        // R5 accessibility: high contrast owns the theme while it is on, so a broker
        // write must not be able to turn it off. Keep the broker agreeing with what
        // is actually applied (unless it already does, so this cannot loop).
        if (Config::accessibility().high_contrast) {
            auto file = Core::File::open(path, Core::File::OpenMode::Read);
            if (file.is_error())
                return;
            auto contents = file.value()->read_until_eof();
            if (contents.is_error())
                return;
            auto requested = StringView { contents.value() }.trim_whitespace().to_byte_string();
            auto const forced = high_contrast_theme_path();
            dbgln("WindowServer: high contrast is on; ignoring the theme broker ('{}')", requested);
            if (requested != forced)
                publish_theme_to_broker(forced);
            return;
        }
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
            // The theme's *name* comes from the host settings, not the broker
            // (which only carries the path). Re-broadcast now that Config holds
            // the applied theme: otherwise clients answer get_system_theme() from
            // the previous broadcast and a theme menu highlights the one before
            // the one that was just selected.
            broadcast_host_settings(server);
        }
    };
    if (auto directory = LexicalPath(path).dirname().to_byte_string(); !directory.is_empty())
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

void watch_fonts_broker(Server& server)
{
    static RefPtr<Core::FileWatcher> watcher;
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto path = fonts_broker_path();
    watcher->on_change = [&server, path](Core::FileWatcherEvent const& event) {
        if (event.event_path != path)
            return;
        read_and_apply_fonts(server);
    };
    if (auto directory = LexicalPath(path).dirname().to_byte_string(); !directory.is_empty())
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

void apply_cursor_state(Server& server)
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

void watch_cursor_state(Server& server)
{
    static RefPtr<Core::FileWatcher> watcher;
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto path = cursor_state_path();
    watcher->on_change = [&server, path](Core::FileWatcherEvent const& event) {
        if (event.event_path != path)
            return;
        apply_cursor_state(server);
    };
    if (auto directory = LexicalPath(path).dirname().to_byte_string(); !directory.is_empty())
        (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    if (Core::File::open(path, Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(path, Core::File::OpenMode::Write);
    (void)watcher->add_watch(path, Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified);
}
