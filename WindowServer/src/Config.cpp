/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Config.h"

#include <AK/Array.h>
#include <AK/Format.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/FileWatcher.h>
#include <LibCore/Timer.h>
#include <stdlib.h>
#include <xkbcommon/xkbcommon.h>

namespace Config {

static Workspaces g_workspaces;
static Tiling g_tiling;
static Focus g_focus;
static Effects g_effects;
static Output g_output;
static Vector<ByteString> g_applet_order;
static Vector<ByteString> g_diagnostics;

// --- keybindings (R4.4) ------------------------------------------------------

struct Binding {
    Action action;
    u32 keysym;
    u32 modifiers;
};

static Vector<Binding> g_bindings;
static constexpr size_t action_count = static_cast<size_t>(Action::_Count);
static Array<ByteString, action_count> g_binding_text;
static Vector<ByteString> g_action_names;

struct BindingSpec {
    StringView name;
    Action action;
    StringView default_combos;
};

// Defaults mirror the compositor's historical hardcoded bindings.
static constexpr BindingSpec s_binding_specs[] = {
    { "SwitchWorkspaceLeft"sv, Action::SwitchWorkspaceLeft, "Ctrl+Alt+Left"sv },
    { "SwitchWorkspaceRight"sv, Action::SwitchWorkspaceRight, "Ctrl+Alt+Right"sv },
    { "SwitchWorkspaceUp"sv, Action::SwitchWorkspaceUp, "Ctrl+Alt+Up"sv },
    { "SwitchWorkspaceDown"sv, Action::SwitchWorkspaceDown, "Ctrl+Alt+Down"sv },
    { "CarryWorkspaceLeft"sv, Action::CarryWorkspaceLeft, "Ctrl+Shift+Alt+Left"sv },
    { "CarryWorkspaceRight"sv, Action::CarryWorkspaceRight, "Ctrl+Shift+Alt+Right"sv },
    { "CarryWorkspaceUp"sv, Action::CarryWorkspaceUp, "Ctrl+Shift+Alt+Up"sv },
    { "CarryWorkspaceDown"sv, Action::CarryWorkspaceDown, "Ctrl+Shift+Alt+Down"sv },
    { "TileLeft"sv, Action::TileLeft, "Super+Left"sv },
    { "TileRight"sv, Action::TileRight, "Super+Right"sv },
    { "Maximize"sv, Action::Maximize, "Super+Up"sv },
    { "Minimize"sv, Action::Minimize, "Super+Down"sv },
    { "MaximizeHorizontal"sv, Action::MaximizeHorizontal, "Super+Alt+Left,Super+Alt+Right"sv },
    { "MaximizeVertical"sv, Action::MaximizeVertical, "Super+Alt+Up,Super+Alt+Down"sv },
    { "Lock"sv, Action::Lock, "Super+L"sv },
};

// "Ctrl+Alt+Left" -> (XKB_KEY_Left, ModCtrl|ModAlt). Returns false if it does not
// name a non-empty modifier set and exactly one keysym.
static bool parse_combo(StringView combo, u32& keysym, u32& modifiers)
{
    keysym = 0;
    modifiers = 0;
    bool saw_key = false;
    for (auto part : combo.split_view('+')) {
        auto token = part.trim_whitespace();
        if (token.is_empty())
            continue;
        if (token == "Ctrl"sv)
            modifiers |= ModCtrl;
        else if (token == "Alt"sv)
            modifiers |= ModAlt;
        else if (token == "Shift"sv)
            modifiers |= ModShift;
        else if (token == "Super"sv || token == "Logo"sv || token == "Meta"sv)
            modifiers |= ModSuper;
        else {
            if (saw_key)
                return false;
            auto key_name = token.to_byte_string();
            keysym = xkb_keysym_from_name(key_name.characters(), XKB_KEYSYM_CASE_INSENSITIVE);
            if (keysym == XKB_KEY_NoSymbol)
                return false;
            saw_key = true;
        }
    }
    return saw_key && modifiers != 0;
}

static void add_bindings(Vector<Binding>& out, Action action, StringView combos)
{
    for (auto combo : combos.split_view(',')) {
        u32 keysym = 0;
        u32 modifiers = 0;
        if (parse_combo(combo.trim_whitespace(), keysym, modifiers))
            out.append({ action, keysym, modifiers });
    }
}


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

// Returns the valued integer if the key is set and valid; records a diagnostic
// (and returns empty, so the previous value is kept) if it is out of range.
static Optional<int> read_int(RefPtr<Core::ConfigFile> const& user, RefPtr<Core::ConfigFile> const& defaults, StringView group, StringView key, int min, int max)
{
    auto raw = read_entry(user, defaults, group, key);
    if (raw.is_empty())
        return {};
    auto value = raw.to_number<int>();
    if (!value.has_value() || value.value() < min || value.value() > max) {
        g_diagnostics.append(ByteString::formatted("[{}] {}='{}' invalid (expected {}-{})", group, key, raw, min, max));
        return {};
    }
    return value;
}

static Optional<bool> read_bool(RefPtr<Core::ConfigFile> const& user, RefPtr<Core::ConfigFile> const& defaults, StringView group, StringView key)
{
    auto raw = read_entry(user, defaults, group, key);
    if (raw.is_empty())
        return {};
    auto lower = raw.to_lowercase();
    if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
        return true;
    if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
        return false;
    g_diagnostics.append(ByteString::formatted("[{}] {}='{}' invalid (expected true/false)", group, key, raw));
    return {};
}

void load()
{
    g_diagnostics.clear();

    auto user = open_config(file_path());
    auto defaults = open_config(default_config_path());

    if (auto value = read_int(user, defaults, "Workspaces"sv, "Rows"sv, 1, 16); value.has_value())
        g_workspaces.rows = value.value();
    if (auto value = read_int(user, defaults, "Workspaces"sv, "Columns"sv, 1, 16); value.has_value())
        g_workspaces.columns = value.value();

    if (auto value = read_int(user, defaults, "Tiling"sv, "EdgeThreshold"sv, 1, 512); value.has_value())
        g_tiling.edge_threshold = value.value();
    if (auto value = read_bool(user, defaults, "Tiling"sv, "DragToEdge"sv); value.has_value())
        g_tiling.drag_to_edge = value.value();

    if (auto value = read_bool(user, defaults, "Focus"sv, "FollowsMouse"sv); value.has_value())
        g_focus.follows_mouse = value.value();

    if (auto value = read_bool(user, defaults, "Effects"sv, "WindowGeometry"sv); value.has_value())
        g_effects.window_geometry = value.value();

    if (auto value = read_int(user, defaults, "Output"sv, "DefaultScale"sv, 0, 4); value.has_value())
        g_output.default_scale = value.value();

    g_applet_order.clear();
    for (auto& entry : read_entry(user, defaults, "Applet"sv, "Order"sv).split(',')) {
        auto trimmed = entry.trim_whitespace();
        if (!trimmed.is_empty())
            g_applet_order.append(trimmed);
    }

    g_bindings.clear();
    g_action_names.clear();
    for (auto const& spec : s_binding_specs) {
        g_action_names.append(spec.name);
        auto text = read_entry(user, defaults, "Keybindings"sv, spec.name);
        if (text.is_empty())
            text = spec.default_combos;
        Vector<Binding> parsed;
        add_bindings(parsed, spec.action, text);
        if (parsed.is_empty()) {
            g_diagnostics.append(ByteString::formatted("[Keybindings] {}='{}' invalid (expected e.g. {})", spec.name, text, spec.default_combos));
            add_bindings(parsed, spec.action, spec.default_combos);
            text = spec.default_combos;
        }
        for (auto& binding : parsed)
            g_bindings.append(binding);
        g_binding_text[static_cast<size_t>(spec.action)] = text;
    }

    for (auto const& diagnostic : g_diagnostics)
        warnln("WindowServer: config: {}", diagnostic);

    dbgln("WindowServer: config: workspaces {}x{}, applets {}, tiling edge {}px (drag {}), follows-mouse {}, geometry {}, output scale {}",
        g_workspaces.rows, g_workspaces.columns, g_applet_order.size(),
        g_tiling.edge_threshold, g_tiling.drag_to_edge ? "on" : "off",
        g_focus.follows_mouse ? "on" : "off", g_effects.window_geometry ? "on" : "off", g_output.default_scale);
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

Tiling const& tiling()
{
    return g_tiling;
}

Focus const& focus()
{
    return g_focus;
}

Effects const& effects()
{
    return g_effects;
}

Output const& output()
{
    return g_output;
}

Vector<ByteString> const& applet_order()
{
    return g_applet_order;
}

Vector<ByteString> const& diagnostics()
{
    return g_diagnostics;
}

Optional<Action> action_for(u32 keysym, u32 modifiers)
{
    for (auto const& binding : g_bindings) {
        if (binding.keysym == keysym && binding.modifiers == modifiers)
            return binding.action;
    }
    return {};
}

ByteString const& binding_text(Action action)
{
    return g_binding_text[static_cast<size_t>(action)];
}

Vector<ByteString> const& action_names()
{
    return g_action_names;
}

}
