/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Config.h"

#include <AK/Array.h>
#include <AK/Format.h>
#include <AK/Math.h>
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
static Log g_log;
static Vector<OutputSettings> g_outputs;
static unsigned g_main_screen { 0 };
static Input g_input;
static InputRanges g_input_ranges;
static Fonts g_fonts;
static Theme g_theme;
static Vector<GlobalShortcut> g_global_shortcuts;
static Vector<ByteString> g_applet_order;
static Vector<ByteString> g_diagnostics;

namespace {
struct PlacementRule {
    ByteString title;
    Gfx::IntRect rect;
};
}
static Vector<PlacementRule> g_placement_rules;

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

// Output scale: 0 = auto, else 0.5..4.0 (fractional allowed, e.g. 1.5).
static Optional<double> read_scale(RefPtr<Core::ConfigFile> const& user, RefPtr<Core::ConfigFile> const& defaults, StringView group, StringView key)
{
    auto raw = read_entry(user, defaults, group, key);
    if (raw.is_empty())
        return {};
    char* end = nullptr;
    double const value = strtod(raw.characters(), &end);
    if (end == raw.characters() || value < 0.0 || (value != 0.0 && (value < 0.5 || value > 4.0))) {
        g_diagnostics.append(ByteString::formatted("[{}] {}='{}' invalid (expected 0 for auto, or 0.5-4.0)", group, key, raw));
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
    g_effects.show_geometry = read_entry(user, defaults, "Effects"sv, "ShowGeometry"sv);
    if (g_effects.show_geometry.is_empty())
        g_effects.show_geometry = "OnMoveAndResize"sv;
    g_effects.tile_window = read_entry(user, defaults, "Effects"sv, "TileWindow"sv);
    if (g_effects.tile_window.is_empty())
        g_effects.tile_window = "ShowTileOverlay"sv;
    if (auto value = read_bool(user, defaults, "Effects"sv, "AnimateWindows"sv); value.has_value())
        g_effects.animate_windows = value.value();

    if (auto value = read_scale(user, defaults, "Output"sv, "DefaultScale"sv); value.has_value())
        g_output.default_scale = value.value();
    g_output.renderer = read_entry(user, defaults, "Output"sv, "Renderer"sv);
    if (!g_output.renderer.is_empty()) {
        // R6/N5: the wlroots renderer selection (see main.cpp).
        if (g_output.renderer != "auto" && g_output.renderer != "gles2" && g_output.renderer != "vulkan" && g_output.renderer != "pixman") {
            g_diagnostics.append(ByteString::formatted("[Output] Renderer='{}' invalid (expected auto|gles2|vulkan|pixman)", g_output.renderer));
            g_output.renderer = {};
        }
    }

    g_log.level = read_entry(user, defaults, "Log"sv, "Level"sv);
    if (!g_log.level.is_empty()) {
        // R6/N8: the compositor's verbosity (see main.cpp).
        if (g_log.level != "error" && g_log.level != "warn" && g_log.level != "info" && g_log.level != "debug") {
            g_diagnostics.append(ByteString::formatted("[Log] Level='{}' invalid (expected error|warn|info|debug)", g_log.level));
            g_log.level = {};
        }
    }

    g_applet_order.clear();
    for (auto& entry : read_entry(user, defaults, "Applet"sv, "Order"sv).split(',')) {
        auto trimmed = entry.trim_whitespace();
        if (!trimmed.is_empty())
            g_applet_order.append(trimmed);
    }
    // SDE owns the Battery and Wi-Fi applets, which are not in the SerenityOS
    // default order. Applets are laid out right-to-left by descending order index
    // (the *last* entry is rightmost), so append Wi-Fi and then Battery unless the
    // user placed them: Wi-Fi sits just to the left of the battery applet.
    if (!g_applet_order.is_empty()) {
        for (auto const& name : { ByteString { "WiFi"sv }, ByteString { "Battery"sv } }) {
            if (!g_applet_order.contains_slow(name))
                g_applet_order.append(name);
        }
    }

    // [Placement] Windows - initial window geometry rules (see Config.h).
    g_placement_rules.clear();
    for (auto& item : read_entry(user, defaults, "Placement"sv, "Windows"sv).split(';')) {
        auto trimmed = item.trim_whitespace();
        if (trimmed.is_empty())
            continue;
        auto colon = trimmed.find(':');
        if (!colon.has_value())
            continue;
        auto title = trimmed.substring(0, *colon).trim_whitespace();
        auto parts = trimmed.substring(*colon + 1).split(',');
        if (parts.size() != 4)
            continue;
        int x = parts[0].trim_whitespace().to_number<int>().value_or(0);
        int y = parts[1].trim_whitespace().to_number<int>().value_or(0);
        int width = parts[2].trim_whitespace().to_number<int>().value_or(0);
        int height = parts[3].trim_whitespace().to_number<int>().value_or(0);
        g_placement_rules.append({ title, Gfx::IntRect { x, y, width, height } });
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

    // Input-setting ranges ([Input]): the source of truth for the settings UI
    // bounds (R4.6).
    g_input_ranges.accel_min = read_entry(user, defaults, "Input"sv, "AccelMin"sv).to_number<double>().value_or(0.5);
    g_input_ranges.accel_max = read_entry(user, defaults, "Input"sv, "AccelMax"sv).to_number<double>().value_or(3.5);
    g_input_ranges.scroll_step_min = read_entry(user, defaults, "Input"sv, "ScrollStepMin"sv).to_number<u32>().value_or(1);
    g_input_ranges.double_click_min = read_entry(user, defaults, "Input"sv, "DoubleClickMin"sv).to_number<int>().value_or(100);
    g_input_ranges.double_click_max = read_entry(user, defaults, "Input"sv, "DoubleClickMax"sv).to_number<int>().value_or(900);

    // System fonts ([Fonts]): persisted so a chosen family/size survives a restart.
    // SDE defaults to the bundled bitmap Katica/Csilla families (as upstream
    // SerenityOS does); picking a scalable family is what enables size zoom.
    g_fonts.default_query = read_entry(user, defaults, "Fonts"sv, "Default"sv);
    if (g_fonts.default_query.is_empty())
        g_fonts.default_query = "Katica 10 400 0"sv;
    g_fonts.fixed_width_query = read_entry(user, defaults, "Fonts"sv, "FixedWidth"sv);
    if (g_fonts.fixed_width_query.is_empty())
        g_fonts.fixed_width_query = "Csilla 10 400 0"sv;
    g_fonts.window_title_query = read_entry(user, defaults, "Fonts"sv, "WindowTitle"sv);
    if (g_fonts.window_title_query.is_empty())
        g_fonts.window_title_query = "Katica 10 700 0"sv;

    // The chosen system theme ([Theme]); the live channel is the theme broker.
    g_theme.name = read_entry(user, defaults, "Theme"sv, "Name"sv);
    if (g_theme.name.is_empty())
        g_theme.name = "Default"sv;
    g_theme.path = read_entry(user, defaults, "Theme"sv, "Path"sv);

    for (auto const& diagnostic : g_diagnostics)
        warnln("WindowServer: config: {}", diagnostic);

    dbgln("WindowServer: config: workspaces {}x{}, applets {}, tiling edge {}px (drag {}), follows-mouse {}, geometry {} (show-geometry '{}', tile-window '{}'), output scale {}",
        g_workspaces.rows, g_workspaces.columns, g_applet_order.size(),
        g_tiling.edge_threshold, g_tiling.drag_to_edge ? "on" : "off",
        g_focus.follows_mouse ? "on" : "off", g_effects.window_geometry ? "on" : "off",
        g_effects.show_geometry, g_effects.tile_window, g_output.default_scale);
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
    // Ensure the file exists so its content can be watched: a directory watch
    // only reports child creation, not the in-place edits we reload on.
    if (Core::File::open(file_path(), Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(file_path(), Core::File::OpenMode::Write);
    (void)watcher->add_watch(file_path(), Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified | Core::FileWatcherEvent::Type::Deleted);
}

Workspaces const& workspaces()
{
    return g_workspaces;
}

// --- screen layout (R4.5) ---------------------------------------------------
//
// The persisted layout uses the Serenity WindowServer ScreenLayout format, so
// the port (LibGUI ScreenLayout::load_config/save_config) and DisplaySettings'
// Monitor tab read and write the same screenlayout.ini the compositor watches.

static ByteString outputs_file()
{
    return state_file("screenlayout.ini"sv);
}

void load_outputs()
{
    g_outputs.clear();
    auto config = open_config(outputs_file());
    if (!config)
        return;
    g_main_screen = config->read_num_entry<unsigned>("Screens"sv, "MainScreen"sv, 0);
    for (size_t index = 0;; ++index) {
        auto group = ByteString::formatted("Screen{}", index);
        if (!config->has_group(group))
            break;
        OutputSettings settings;
        settings.mode = config->read_entry(group, "Mode"sv, "Device"sv);
        settings.device = config->read_entry(group, "Device"sv);
        settings.left = config->read_num_entry<int>(group, "Left"sv, 0);
        settings.top = config->read_num_entry<int>(group, "Top"sv, 0);
        settings.width = config->read_num_entry<int>(group, "Width"sv, 0);
        settings.height = config->read_num_entry<int>(group, "Height"sv, 0);
        settings.scale = config->read_num_entry<int>(group, "ScaleFactor"sv, 1);
        g_outputs.append(move(settings));
    }
    dbgln("WindowServer: config: {} screen(s) in screenlayout.ini (main {})", g_outputs.size(), g_main_screen);
}

void set_outputs(Vector<OutputSettings> outputs, unsigned main_screen_index)
{
    g_outputs = move(outputs);
    g_main_screen = main_screen_index;
    (void)Core::Directory::create(state_directory(), Core::Directory::CreateDirectories::Yes);
    auto config = Core::ConfigFile::open(outputs_file(), Core::ConfigFile::AllowWriting::Yes).release_value_but_fixme_should_propagate_errors();
    config->write_num_entry("Screens"sv, "MainScreen"sv, g_main_screen);
    for (size_t index = 0; index < g_outputs.size(); ++index) {
        auto group = ByteString::formatted("Screen{}", index);
        auto const& settings = g_outputs[index];
        config->write_entry(group, "Mode"sv, settings.mode);
        if (settings.mode == "Device"sv)
            config->write_entry(group, "Device"sv, settings.device);
        config->write_num_entry(group, "Left"sv, settings.left);
        config->write_num_entry(group, "Top"sv, settings.top);
        config->write_num_entry(group, "Width"sv, settings.width);
        config->write_num_entry(group, "Height"sv, settings.height);
        config->write_num_entry(group, "ScaleFactor"sv, settings.scale);
    }
    // Prune screens past the end of the layout.
    for (size_t index = g_outputs.size();; ++index) {
        auto group = ByteString::formatted("Screen{}", index);
        if (!config->has_group(group))
            break;
        config->remove_group(group);
    }
    (void)config->sync();
}

void watch_outputs(Function<void()> on_change)
{
    static RefPtr<Core::FileWatcher> watcher;
    static RefPtr<Core::Timer> coalesce;
    coalesce = Core::Timer::create_single_shot(30, [on_change = move(on_change)] {
        load_outputs();
        on_change();
    });
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto directory = state_directory();
    watcher->on_change = [directory](Core::FileWatcherEvent const& event) {
        if (!event.event_path.starts_with(directory))
            return;
        coalesce->restart();
    };
    (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    if (Core::File::open(outputs_file(), Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(outputs_file(), Core::File::OpenMode::Write);
    (void)watcher->add_watch(outputs_file(), Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified | Core::FileWatcherEvent::Type::Deleted);
}

Vector<OutputSettings> const& outputs()
{
    return g_outputs;
}

unsigned main_screen_index()
{
    return g_main_screen;
}

// --- input devices (R4.6) ---------------------------------------------------

static ByteString input_file()
{
    return state_file("input.ini"sv);
}

static bool read_bool_entry(Core::ConfigFile& config, StringView group, StringView key, bool default_value)
{
    auto value = config.read_entry(group, key);
    if (value.is_empty())
        return default_value;
    return value == "true"sv || value == "1"sv || value == "yes"sv || value == "on"sv;
}

void load_input()
{
    Input input;
    auto config = open_config(input_file());
    if (config) {
        auto& file = *config;
        input.mouse.acceleration = file.read_entry("Mouse"sv, "Acceleration"sv, "1.0"sv).to_number<double>().value_or(1.0);
        input.mouse.scroll_step_size = file.read_num_entry<u32>("Mouse"sv, "ScrollStepSize"sv, 1);
        input.mouse.double_click_speed = file.read_num_entry<int>("Mouse"sv, "DoubleClickSpeed"sv, 250);
        input.mouse.buttons_switched = read_bool_entry(file, "Mouse"sv, "ButtonsSwitched"sv, false);
        input.mouse.natural_scroll = read_bool_entry(file, "Mouse"sv, "NaturalScroll"sv, false);
        input.mouse.accel_profile = file.read_entry("Mouse"sv, "AccelProfile"sv, "adaptive"sv);
        input.mouse.tap_to_click = read_bool_entry(file, "Mouse"sv, "TapToClick"sv, false);
        input.mouse.left_handed = read_bool_entry(file, "Mouse"sv, "LeftHanded"sv, false);
        input.mouse.disable_while_typing = read_bool_entry(file, "Mouse"sv, "DisableWhileTyping"sv, true);
        input.keyboard.layouts = file.read_entry("Keyboard"sv, "Layouts"sv, "us"sv);
        input.keyboard.variant = file.read_entry("Keyboard"sv, "Variant"sv);
        input.keyboard.model = file.read_entry("Keyboard"sv, "Model"sv, "pc105"sv);
        input.keyboard.options = file.read_entry("Keyboard"sv, "Options"sv);
        input.keyboard.repeat_delay = file.read_num_entry<int>("Keyboard"sv, "RepeatDelay"sv, 600);
        input.keyboard.repeat_rate = file.read_num_entry<int>("Keyboard"sv, "RepeatRate"sv, 25);
    }
    // Keep the values within the schema's ranges ([Input] in WindowServer.ini).
    if (g_input_ranges.accel_min <= g_input_ranges.accel_max)
        input.mouse.acceleration = clamp(input.mouse.acceleration, g_input_ranges.accel_min, g_input_ranges.accel_max);
    input.mouse.scroll_step_size = max(input.mouse.scroll_step_size, g_input_ranges.scroll_step_min);
    if (g_input_ranges.double_click_min <= g_input_ranges.double_click_max)
        input.mouse.double_click_speed = clamp(input.mouse.double_click_speed, g_input_ranges.double_click_min, g_input_ranges.double_click_max);
    g_input = move(input);
    dbgln("WindowServer: config: input accel {}, scroll step {}, double-click {}ms, buttons-switched {}, natural-scroll {}, tap {}, left-handed {}, layouts '{}'",
        g_input.mouse.acceleration, g_input.mouse.scroll_step_size, g_input.mouse.double_click_speed,
        g_input.mouse.buttons_switched ? "on" : "off", g_input.mouse.natural_scroll ? "on" : "off",
        g_input.mouse.tap_to_click ? "on" : "off", g_input.mouse.left_handed ? "on" : "off", g_input.keyboard.layouts);
}

Input const& input()
{
    return g_input;
}

InputRanges const& input_ranges()
{
    return g_input_ranges;
}

Fonts const& fonts()
{
    return g_fonts;
}

void set_fonts(Fonts fonts)
{
    g_fonts = move(fonts);
    (void)Core::Directory::create(state_directory(), Core::Directory::CreateDirectories::Yes);
    auto config = Core::ConfigFile::open(file_path(), Core::ConfigFile::AllowWriting::Yes).release_value_but_fixme_should_propagate_errors();
    config->write_entry("Fonts"sv, "Default"sv, g_fonts.default_query);
    if (!g_fonts.fixed_width_query.is_empty())
        config->write_entry("Fonts"sv, "FixedWidth"sv, g_fonts.fixed_width_query);
    if (!g_fonts.window_title_query.is_empty())
        config->write_entry("Fonts"sv, "WindowTitle"sv, g_fonts.window_title_query);
    (void)config->sync();
}

Theme const& theme()
{
    return g_theme;
}

void set_theme(Theme theme)
{
    g_theme = move(theme);
    (void)Core::Directory::create(state_directory(), Core::Directory::CreateDirectories::Yes);
    auto config = Core::ConfigFile::open(file_path(), Core::ConfigFile::AllowWriting::Yes).release_value_but_fixme_should_propagate_errors();
    config->write_entry("Theme"sv, "Name"sv, g_theme.name);
    if (!g_theme.path.is_empty())
        config->write_entry("Theme"sv, "Path"sv, g_theme.path);
    (void)config->sync();
}

void watch_input(Function<void()> on_change)
{
    static RefPtr<Core::FileWatcher> watcher;
    static RefPtr<Core::Timer> coalesce;
    coalesce = Core::Timer::create_single_shot(30, [on_change = move(on_change)] {
        load_input();
        on_change();
    });
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto directory = state_directory();
    watcher->on_change = [directory](Core::FileWatcherEvent const& event) {
        if (!event.event_path.starts_with(directory))
            return;
        coalesce->restart();
    };
    (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    if (Core::File::open(input_file(), Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(input_file(), Core::File::OpenMode::Write);
    (void)watcher->add_watch(input_file(), Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified | Core::FileWatcherEvent::Type::Deleted);
}

// --- global shortcuts (R5) --------------------------------------------------

static ByteString global_shortcuts_file()
{
    return state_file("global-shortcuts.ini"sv);
}

void load_global_shortcuts()
{
    g_global_shortcuts.clear();
    auto config = open_config(global_shortcuts_file());
    if (!config)
        return;
    for (auto const& group : config->groups()) {
        auto trigger = config->read_entry(group, "Trigger"sv);
        auto session = config->read_entry(group, "Session"sv);
        auto id = config->read_entry(group, "Id"sv);
        if (trigger.is_empty() || session.is_empty() || id.is_empty())
            continue;
        u32 keysym = 0;
        u32 modifiers = 0;
        if (!parse_combo(trigger, keysym, modifiers))
            continue;
        GlobalShortcut shortcut;
        shortcut.keysym = keysym;
        shortcut.modifiers = modifiers;
        shortcut.session = session;
        shortcut.id = id;
        g_global_shortcuts.append(move(shortcut));
    }
    dbgln("WindowServer: config: {} global shortcut(s)", g_global_shortcuts.size());
}

Vector<GlobalShortcut> const& global_shortcuts()
{
    return g_global_shortcuts;
}

void watch_global_shortcuts(Function<void()> on_change)
{
    static RefPtr<Core::FileWatcher> watcher;
    static RefPtr<Core::Timer> coalesce;
    coalesce = Core::Timer::create_single_shot(30, [on_change = move(on_change)] {
        load_global_shortcuts();
        on_change();
    });
    watcher = Core::FileWatcher::create().release_value_but_fixme_should_propagate_errors();
    auto directory = state_directory();
    watcher->on_change = [directory](Core::FileWatcherEvent const& event) {
        if (!event.event_path.starts_with(directory))
            return;
        coalesce->restart();
    };
    (void)watcher->add_watch(directory, Core::FileWatcherEvent::Type::ChildCreated);
    if (Core::File::open(global_shortcuts_file(), Core::File::OpenMode::Read).is_error())
        (void)Core::File::open(global_shortcuts_file(), Core::File::OpenMode::Write);
    (void)watcher->add_watch(global_shortcuts_file(), Core::FileWatcherEvent::Type::ContentModified | Core::FileWatcherEvent::Type::MetadataModified | Core::FileWatcherEvent::Type::Deleted);
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

Log const& log()
{
    return g_log;
}

Vector<ByteString> const& applet_order()
{
    return g_applet_order;
}

Optional<Gfx::IntRect> window_placement_for(StringView title)
{
    for (auto const& rule : g_placement_rules) {
        if (title.contains(rule.title.view()))
            return rule.rect;
    }
    return {};
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
