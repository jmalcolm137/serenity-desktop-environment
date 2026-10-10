/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Config.h"

#include <AK/Format.h>
#include <AK/StringView.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/Timer.h>
#include <stdlib.h>

namespace Config {

static Workspaces g_workspaces;
static Vector<ByteString> g_applet_order;

ByteString state_directory()
{
    if (auto const* dir = getenv("SDE_STATE_DIR"); dir && *dir)
        return ByteString(dir);
    if (auto const* config = getenv("XDG_CONFIG_HOME"); config && *config)
        return ByteString::formatted("{}/sde", config);
    if (auto const* home = getenv("HOME"); home && *home)
        return ByteString::formatted("{}/.config/sde", home);
    return ByteString { "/tmp/sde" };
}

ByteString state_file(StringView name)
{
    return ByteString::formatted("{}/{}", state_directory(), name);
}

ByteString read_state(StringView name)
{
    auto file = Core::File::open(state_file(name), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return StringView { contents.value() }.trim_whitespace().to_byte_string();
}

ByteBuffer read_state_bytes(StringView name)
{
    auto file = Core::File::open(state_file(name), Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return contents.release_value();
}

void write_state(StringView name, ReadonlyBytes bytes)
{
    (void)Core::Directory::create(state_directory(), Core::Directory::CreateDirectories::Yes);
    if (auto file = Core::File::open(state_file(name), Core::File::OpenMode::Write); !file.is_error())
        (void)file.value()->write_until_depleted(bytes);
}

ByteString file_path()
{
    return state_file("WindowServer.ini"sv);
}

static ByteString default_config_path()
{
    // The SerenityOS defaults ship with the resource tree.
    if (auto const* root = getenv("SERENITY_RES_ROOT"); root && *root)
        return ByteString::formatted("{}/../etc/WindowServer.ini", root);
    return ByteString { "/etc/WindowServer.ini" };
}

static RefPtr<Core::ConfigFile> open_config(ByteString const& path)
{
    auto config = Core::ConfigFile::open(path);
    if (config.is_error())
        return nullptr;
    return config.release_value();
}

static ByteString read_entry(RefPtr<Core::ConfigFile> const& user, RefPtr<Core::ConfigFile> const& defaults, StringView group, StringView key)
{
    for (auto const& config : { user, defaults }) {
        if (!config)
            continue;
        auto value = config->read_entry(group, key);
        if (!value.is_empty())
            return value.trim_whitespace();
    }
    return {};
}

void load()
{
    auto user = open_config(file_path());
    auto defaults = open_config(default_config_path());

    g_workspaces.rows = read_entry(user, defaults, "Workspaces"sv, "Rows"sv).to_number<int>().value_or(2);
    g_workspaces.columns = read_entry(user, defaults, "Workspaces"sv, "Columns"sv).to_number<int>().value_or(2);

    g_applet_order.clear();
    for (auto& entry : read_entry(user, defaults, "Applet"sv, "Order"sv).split(',')) {
        auto trimmed = entry.trim_whitespace();
        if (!trimmed.is_empty())
            g_applet_order.append(trimmed);
    }

    dbgln("WindowServer: config: workspaces {}x{}, {} applets ordered", g_workspaces.rows, g_workspaces.columns, g_applet_order.size());
}

void watch(Function<void()> on_reload)
{
    static RefPtr<Core::FileWatcher> watcher;
    static RefPtr<Core::Timer> coalesce;

    // One edit produces several inotify events; coalesce them into one reload.
    coalesce = Core::Timer::create_single_shot(30, [on_reload = move(on_reload)] {
        load();
        on_reload();
    });

    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto directory = state_directory();
    watcher->on_change = [directory](Core::FileWatcherEvent const& event) {
        if (!event.event_path.starts_with(directory))
            return;
        coalesce->restart();
    };
    (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    (void)watcher->add_watch(file_path(), Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified | Core::FileWatcherEvent::Type::Deleted);
}

Workspaces const& workspaces()
{
    return g_workspaces;
}

Vector<ByteString> const& applet_order()
{
    return g_applet_order;
}

}
