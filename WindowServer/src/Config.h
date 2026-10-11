/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/Vector.h>

// The session's persisted configuration and state, rooted at
// $XDG_CONFIG_HOME/sde (SDE_STATE_DIR overrides). The wallpaper/theme state the
// Display Settings app writes and the compositor config (WindowServer.ini) share
// this one directory, so features add keys here rather than inventing a second
// mechanism. See docs/14-r4.4-config.md for the schema.
//
// WindowServer.ini schema (all keys optional; the SerenityOS defaults under
// $SERENITY_RES_ROOT/../etc are overlaid by the user's file):
//
//   [Workspaces] Rows, Columns          1..16   (window-stack grid)
//   [Applet]     Order                          (comma-separated)
//   [Tiling]     EdgeThreshold          1..512  (px from an edge that arms snap)
//                DragToEdge             bool    (drag a window to an edge to snap)
//   [Focus]      FollowsMouse           bool    (focus on hover, not just click)
//   [Effects]    WindowGeometry        bool     (geometry HUD while moving/resizing)
//   [Output]     DefaultScale          0..4      (0 = auto integer HiDPI scale)
//   [Keybindings] <Action>              combo(s)  (e.g. Ctrl+Alt+Left; see Action)
//
// Invalid values are rejected (the previous value is kept) and reported through
// diagnostics(); see `scripts/sde-config validate`.
namespace Config {

// --- state directory (shared with the wallpaper/theme state) ---------------

ByteString state_directory();
ByteString state_file(StringView name);

ByteString read_state(StringView name);
ByteBuffer read_state_bytes(StringView name);
void write_state(StringView name, ReadonlyBytes bytes);

// --- compositor config (WindowServer.ini) -----------------------------------

// The window-stack grid (SerenityOS default 2x2).
struct Workspaces {
    int rows { 2 };
    int columns { 2 };
};

// Snap/tile behaviour (WS2).
struct Tiling {
    int edge_threshold { 48 };
    bool drag_to_edge { true };
};

// Focus policy.
struct Focus {
    bool follows_mouse { false };
};

// System effects (SerenityOS SystemEffects subset).
struct Effects {
    bool window_geometry { true };
};

// Output defaults.
struct Output {
    int default_scale { 0 }; // 0 = auto (integer HiDPI)
};

// A persisted screen entry (screenlayout.ini in the state directory). This is
// the Serenity WindowServer ScreenLayout format ([Screens] MainScreen, then
// [Screen0] Mode/Device/Left/Top/Width/Height/ScaleFactor) so the port's
// ScreenLayout::load_config/save_config and DisplaySettings' Monitor tab read
// and write the same file the compositor watches (R4.5).
struct OutputSettings {
    ByteString mode { "Device"sv }; // "Device" or "Virtual"
    ByteString device;              // output name (Mode == Device)
    int left { 0 };
    int top { 0 };
    int width { 0 };
    int height { 0 };
    int scale { 1 };
};

// Load the effective configuration: the SerenityOS defaults
// ($SERENITY_RES_ROOT/../etc/WindowServer.ini) overlaid by the user's file in the
// state directory. Invalid values are rejected and recorded in diagnostics().
void load();

// Watch the config file and re-load on change; on_reload() runs after each load.
// A single edit produces several inotify events, so reloads are coalesced.
void watch(Function<void()> on_reload);

Workspaces const& workspaces();

// [Applet] Order - the configured applet order (comma-separated). WS4 reads this.
Vector<ByteString> const& applet_order();

Tiling const& tiling();
Focus const& focus();
Effects const& effects();
Output const& output();

// The persisted screen layout (R4.5), in the Serenity ScreenLayout format
// (screenlayout.ini). outputs() is the desired layout; set_outputs() replaces it
// and writes the file; load_outputs() reads it back.
Vector<OutputSettings> const& outputs();
unsigned main_screen_index();
void set_outputs(Vector<OutputSettings> outputs, unsigned main_screen_index = 0);
void load_outputs();

// Watch screenlayout.ini and call on_change after it is re-loaded.
void watch_outputs(Function<void()> on_change);

// Warnings from the last load() (invalid values, etc.), one per issue. Empty when
// the config is clean. `sde-config validate` prints these.
Vector<ByteString> const& diagnostics();

// The user's config file path, for tooling and error messages.
ByteString file_path();

// --- keybindings (R4.4) ------------------------------------------------------

// Modifier bits, independent of wlroots (main.cpp maps WLR_MODIFIER_* to these).
enum Modifier : u32 {
    ModCtrl = 1 << 0,
    ModAlt = 1 << 1,
    ModShift = 1 << 2,
    ModSuper = 1 << 3,
};

// The compositor actions a keybinding can be assigned to.
enum class Action {
    SwitchWorkspaceLeft,
    SwitchWorkspaceRight,
    SwitchWorkspaceUp,
    SwitchWorkspaceDown,
    CarryWorkspaceLeft,
    CarryWorkspaceRight,
    CarryWorkspaceUp,
    CarryWorkspaceDown,
    TileLeft,
    TileRight,
    Maximize,
    Minimize,
    MaximizeHorizontal,
    MaximizeVertical,
    Lock,
    _Count,
};

// The action bound to (keysym, our Modifier mask), if any.
Optional<Action> action_for(u32 keysym, u32 modifiers);

// The effective combo string for an action (e.g. "Ctrl+Alt+Left"), for tooling.
ByteString const& binding_text(Action);

// Action names in declaration order, for tooling (sde-config list).
Vector<ByteString> const& action_names();

// --- input devices (R4.6) ---------------------------------------------------

// Mouse/pointer settings, persisted in input.ini in the state directory. The
// protocol-backed ones (acceleration, scroll step, double-click speed, buttons
// switched, natural scroll) are also written by MouseSettings via the port.
struct MouseInput {
    double acceleration { 1.0 };
    u32 scroll_step_size { 1 };
    int double_click_speed { 250 };
    bool buttons_switched { false };
    bool natural_scroll { false };
    ByteString accel_profile { "adaptive"sv }; // "adaptive" or "flat"
    bool tap_to_click { false };
    bool left_handed { false };
    bool disable_while_typing { true };
};

// Keyboard settings (xkb): layouts/variant/model/options plus repeat.
struct KeyboardInput {
    ByteString layouts { "us"sv };
    ByteString variant;
    ByteString model { "pc105"sv };
    ByteString options;
    int repeat_delay { 600 };
    int repeat_rate { 25 };
};

struct Input {
    MouseInput mouse;
    KeyboardInput keyboard;
};

// Bounds for the input settings, so the settings UI (Mouse Settings) reads its
// slider/spinbox ranges from the schema instead of hardcoding them. Persisted in
// WindowServer.ini [Input], and served to clients by the port.
struct InputRanges {
    double accel_min { 0.5 };
    double accel_max { 3.5 };
    u32 scroll_step_min { 1 };
    int double_click_min { 100 };
    int double_click_max { 900 };
};

InputRanges const& input_ranges();

Input const& input();
void load_input();
void watch_input(Function<void()> on_change);

}
