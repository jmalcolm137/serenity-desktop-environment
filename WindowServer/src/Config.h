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
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>

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
//   [Output]     DefaultScale          0 or 0.5..4 (0 = auto integer HiDPI; fractional allowed)
//                Renderer              name      (auto|gles2|vulkan|pixman; R6/N5)
//   [Log]        Level                 name      (error|warn|info|debug; R6/N8)
//   [Compositor] XWayland              bool      (run X11 clients via Xwayland; default true)
//   [Taskbar]    GroupWindows          bool      (group a Taskbar button per app id; default false)
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
    int edge_threshold { 16 };
    bool drag_to_edge { true };
};

// Focus policy.
struct Focus {
    bool follows_mouse { false };
};

// System effects (SerenityOS SystemEffects subset).
struct Effects {
    bool window_geometry { true };
    // From WindowServer/SystemEffects.h (ShowGeometryTools/TileWindowTools names),
    // so the Effects settings tab and the compositor agree (R4.7).
    ByteString show_geometry { "OnMoveAndResize"sv };
    ByteString tile_window { "ShowTileOverlay"sv };
    // Animate minimize/restore (the SerenityOS outline from the window to its
    // Taskbar button). Off = no animation, matching the Effects tab.
    bool animate_windows { true };
};

// Output defaults.
struct Output {
    // 0 = auto (integer HiDPI scale), else an explicit output scale (0.5–4.0;
    // fractional allowed, e.g. 1.5). The SDE_OUTPUT_SCALE environment variable
    // overrides it.
    double default_scale { 0 };
    // Requested wlroots renderer: auto|gles2|vulkan|pixman (empty = auto). The
    // SDE_RENDERER environment variable overrides it (R6/N5).
    ByteString renderer;
};

// [Log] Level - the compositor's verbosity (error|warn|info|debug), R6/N8.
struct Log {
    ByteString level;
};

// [Compositor] - compositor feature toggles.
struct Compositor {
    // Run Xwayland so X11 clients work (R5). false disables X11 support entirely
    // (no Xwayland server, no DISPLAY).
    bool xwayland { true };
};

// [Taskbar] - Taskbar app behaviour the compositor conveys to the Taskbar.
struct Taskbar {
    // Group a window's Taskbar button with the other windows of the same
    // application (its app id: an xdg app_id / an X11 WM_CLASS). Off by default,
    // matching SerenityOS's Taskbar (one button per window); a Plasma-style
    // opt-in. Pushed to the Taskbar over the WindowManagerServer protocol.
    bool group_windows { false };
};

// [Accessibility] - assistive input and presentation (docs/29-accessibility.md).
struct Accessibility {
    // Sticky keys: a modifier pressed and released on its own latches, so the next
    // key press is delivered with it (Shift then 'a' produces 'A'), and clears once
    // that key is pressed. Off by default.
    bool sticky_keys { false };
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

// [Placement] Windows - initial window geometry rules (window rules):
//   Windows=<title>:X,Y,W,H;<title>:X,Y,W,H;...
// The first rule whose title is a substring of a new window's title wins, so
// e.g. "File Manager" matches "<dir> - File Manager". Used to reproduce a
// specific desktop layout (e.g. scripts/run-desktop-scene.sh).
Optional<Gfx::IntRect> window_placement_for(StringView title);

Tiling const& tiling();
Focus const& focus();
Effects const& effects();
Output const& output();
Log const& log();
Compositor const& compositor();
Taskbar const& taskbar();
Accessibility const& accessibility();

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

// System fonts ([Fonts] in WindowServer.ini): the queries the compositor and
// clients resolve fonts from (R4.7). Written when the session font broker
// changes.
struct Fonts {
    ByteString default_query;
    ByteString fixed_width_query;
    ByteString window_title_query;
};

Fonts const& fonts();
void set_fonts(Fonts fonts);

// The chosen system theme ([Theme] in WindowServer.ini), persisted so it survives
// a restart; the live channel is the theme broker ($XDG_RUNTIME_DIR/sde-theme).
struct Theme {
    ByteString name { "Default"sv };
    ByteString path;
};
Theme const& theme();
void set_theme(Theme theme);

Input const& input();
void load_input();
void watch_input(Function<void()> on_change);

// --- global shortcuts (R5) --------------------------------------------------
//
// Shortcuts an app requested through the GlobalShortcuts portal, stored in
// global-shortcuts.ini by the backend and bound by the compositor. When one
// fires, the compositor reports it on the session FIFO the backend watches.
struct GlobalShortcut {
    u32 keysym { 0 };
    u32 modifiers { 0 };
    ByteString session;
    ByteString id;
};

Vector<GlobalShortcut> const& global_shortcuts();
void load_global_shortcuts();
void watch_global_shortcuts(Function<void()> on_change);

}
