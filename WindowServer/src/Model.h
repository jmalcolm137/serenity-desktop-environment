/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

#include "Overlays.h" // ChromeNode

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wayland-server-core.h> // wl_listener, wl_resource, wl_display
#include <wlr/util/box.h>        // wlr_box (held by value below)
}

// wlroots pointer types the compositor model refers to. Defining them here, in
// the compositor's own header, means the policy modules only pull in wlroots
// where they actually use it (D5: keep wlroots behind the platform layer).
struct wlr_allocator;
struct wlr_backend;
struct wlr_cursor;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_foreign_toplevel_manager_v1;
struct wlr_keyboard;
struct wlr_layer_shell_v1;
struct wlr_layer_surface_v1;
struct wlr_output;
struct wlr_output_layout;
struct wlr_output_manager_v1;
struct wlr_renderer;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_scene_layer_surface_v1;
struct wlr_scene_output_layout;
struct wlr_scene_tree;
struct wlr_seat;
struct wlr_surface;
struct wlr_virtual_keyboard_manager_v1;
struct wlr_virtual_pointer_manager_v1;
struct wlr_xcursor_manager;
struct wlr_xdg_popup;
struct wlr_xdg_shell;
struct wlr_xdg_toplevel;
struct wlr_xdg_toplevel_decoration_v1;

// The compositor's core model: the server, its outputs, windows and inputs.
// Shared by main.cpp (wiring) and the policy modules (WindowManager, ...).

enum class CursorMode {
    Passthrough,
    Move,
    Resize,
};

// SerenityOS WindowTileType: how a window is snapped to the output.
enum class WindowTileType {
    None,
    Left,
    Right,
    Top,
    Bottom,
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
    VerticallyMaximized,
    HorizontallyMaximized,
    Maximized,
};

struct Server;
struct Output;
struct Toplevel;

// The window menu shown by right-clicking a titlebar, mirroring SerenityOS's
// Window::ensure_window_menu().
enum class WindowMenuAction {
    MinimizeOrUnminimize,
    MaximizeOrRestore,
    Move,
    ToggleWindowRollUp,
    ToggleMenubarVisibility,
    ToggleAlwaysOnTop,
    Close,
};

struct WindowMenuEntry {
    bool is_separator { false };
    ByteString text; // Ampersands mark the keyboard shortcut, as in SerenityOS.
    WindowMenuAction action { WindowMenuAction::Close };
    bool enabled { true };
    bool checkable { false };
    bool checked { false };
    bool is_default { false };
    RefPtr<Gfx::Bitmap> icon;
    Gfx::IntRect rect; // Relative to the menu's top-left.
};

struct WindowMenu {
    bool open { false };
    // Set while the button press that opened the menu is still waiting for its
    // release; that release must not dismiss the freshly opened menu.
    bool ignore_release { false };
    Toplevel* toplevel { nullptr };
    Vector<WindowMenuEntry> entries;
    Gfx::IntRect rect; // Screen coordinates.
    int hovered_index { -1 };
    int pressed_index { -1 };
    OwnPtr<ChromeNode> node; // The menu's scene node, in overlay_tree.
};

struct Output {
    Server* server { nullptr };
    wlr_output* output { nullptr };
    wlr_box usable_area { };
    wlr_scene_buffer* background { nullptr };
    Gfx::IntSize background_size;
    wl_listener frame { };
    wl_listener request_state { };
    wl_listener destroy { };
};

struct Toplevel {
    Server* server { nullptr };
    wlr_xdg_toplevel* xdg_toplevel { nullptr };
    wlr_scene_tree* scene_tree { nullptr };
    wlr_scene_tree* content_tree { nullptr };
    wlr_scene_buffer* frame_buffer { nullptr };
    int frame_left { 0 };
    int frame_top { 0 };
    int frame_right { 0 };
    int frame_bottom { 0 };
    Gfx::IntSize frame_size;
    Vector<Gfx::IntRect> button_rects;
    Gfx::IntSize restore_size;
    Gfx::IntPoint restore_position;
    bool frame_active { true };
    bool maximized { false };
    bool minimized { false };
    WindowTileType tile_type { WindowTileType::None };
    bool fullscreen { false };
    // Minimized specifically by "Show Desktop"; restored by toggling it again.
    bool hidden_by_show_desktop { false };
    bool always_on_top { false };
    bool rolled_up { false };
    int hovered_button { -1 };
    int pressed_button { -1 };
    Gfx::IntRect titlebar_icon_rect;
    wl_resource* chrome { nullptr };
    // The decoration object, if any; the server-side mode is applied once the
    // surface is initialized (set_mode schedules a configure).
    wlr_xdg_toplevel_decoration_v1* decoration { nullptr };
    bool menubar_visible { true };
    // The window stack (workspace grid cell) this window belongs to. WS1.
    u32 workspace_row { 0 };
    u32 workspace_column { 0 };
    i32 wm_id { 0 };
    wlr_foreign_toplevel_handle_v1* foreign_handle { nullptr };
    wl_listener map { };
    wl_listener unmap { };
    wl_listener commit { };
    wl_listener destroy { };
    wl_listener request_move { };
    wl_listener request_resize { };
    wl_listener request_maximize { };
    wl_listener request_fullscreen { };
    wl_listener set_title { };
    wl_listener set_app_id { };
    wl_listener foreign_request_activate { };
    wl_listener foreign_request_close { };
};

struct Popup {
    Server* server { nullptr };
    wlr_xdg_popup* xdg_popup { nullptr };
    wl_listener commit { };
    wl_listener destroy { };
};

struct LayerSurface {
    Server* server { nullptr };
    wlr_layer_surface_v1* layer_surface { nullptr };
    wlr_scene_layer_surface_v1* scene { nullptr };
    wlr_output* output { nullptr };
    // True for client menu popups (the "serenity-menu" namespace): these take no
    // input grab, so an outside press must dismiss them (see cursor_button).
    bool is_menu { false };
    wl_listener map { };
    wl_listener unmap { };
    wl_listener commit { };
    wl_listener destroy { };
};

struct Keyboard {
    Server* server { nullptr };
    wlr_keyboard* keyboard { nullptr };
    wl_listener modifiers { };
    wl_listener key { };
    wl_listener destroy { };
};

struct Decoration {
    Server* server { nullptr };
    Toplevel* toplevel { nullptr };
    wlr_xdg_toplevel_decoration_v1* decoration { nullptr };
    wl_listener request_mode { };
    wl_listener destroy { };
};

struct Server {
    wl_display* display { nullptr };
    wlr_backend* backend { nullptr };
    wlr_renderer* renderer { nullptr };
    wlr_allocator* allocator { nullptr };
    wlr_scene* scene { nullptr };
    wlr_scene_output_layout* scene_layout { nullptr };
    wlr_output_layout* output_layout { nullptr };
    // Per-layer scene trees, in bottom-to-top order, so layer surfaces stack
    // correctly relative to ordinary windows (the flat scene root does not).
    wlr_scene_tree* background_tree { nullptr };
    wlr_scene_tree* bottom_tree { nullptr };
    wlr_scene_tree* windows_tree { nullptr };
    wlr_scene_tree* top_tree { nullptr };
    wlr_scene_tree* overlay_tree { nullptr };
    wlr_xdg_shell* xdg_shell { nullptr };
    wlr_layer_shell_v1* layer_shell { nullptr };
    wlr_foreign_toplevel_manager_v1* foreign_toplevel_manager { nullptr };
    wlr_output_manager_v1* output_manager { nullptr };
    // Virtual input (wlr virtual-keyboard / virtual-pointer), so headless tests
    // can script keyboard and pointer events. See docs/09 §2.4.
    wlr_virtual_keyboard_manager_v1* virtual_keyboard_manager { nullptr };
    wlr_virtual_pointer_manager_v1* virtual_pointer_manager { nullptr };
    wlr_cursor* cursor { nullptr };
    wlr_xcursor_manager* cursor_manager { nullptr };
    wlr_seat* seat { nullptr };
    wlr_output* primary_output { nullptr };

    wl_listener new_output { };
    wl_listener new_xdg_toplevel { };
    wl_listener new_xdg_popup { };
    wl_listener new_layer_surface { };
    wl_listener new_toplevel_decoration { };
    wl_listener output_manager_apply { };
    wl_listener output_manager_test { };
    wl_listener new_input { };
    wl_listener cursor_motion { };
    wl_listener cursor_motion_absolute { };
    wl_listener cursor_button { };
    wl_listener cursor_axis { };
    wl_listener cursor_frame { };
    wl_listener seat_request_cursor { };
    wl_listener seat_pointer_focus_change { };
    wl_listener seat_request_set_selection { };
    wl_listener new_virtual_keyboard { };
    wl_listener new_virtual_pointer { };

    Vector<NonnullOwnPtr<Output>> outputs;
    Vector<NonnullOwnPtr<Toplevel>> toplevels;
    Vector<NonnullOwnPtr<Popup>> popups;
    Vector<NonnullOwnPtr<LayerSurface>> layer_surfaces;
    Vector<NonnullOwnPtr<Keyboard>> keyboards;
    Vector<NonnullOwnPtr<Decoration>> decorations;
    // serenity_window_manager resources, so the compositor can ask a client to
    // dismiss its menus (see maybe_dismiss_layer_menus).
    Vector<wl_resource*> serenity_window_managers;

    CursorMode cursor_mode { CursorMode::Passthrough };
    Toplevel* grabbed_toplevel { nullptr };
    double grab_x { 0 };
    double grab_y { 0 };
    wlr_box grab_box { };
    uint32_t resize_edges { 0 };
    bool frame_grab { false };
    bool show_desktop { false };

    // Implicit pointer grab: while a client button is held, keep the pointer
    // focused on the surface that received the press and forward motion to it,
    // even if the cursor crosses into another surface. wlroots' seat forgets the
    // pressed button when pointer focus changes, so without this the eventual
    // release is silently dropped and the client stays stuck mid-press.
    wlr_surface* pointer_grab_surface { nullptr };
    double pointer_grab_origin_x { 0 };
    double pointer_grab_origin_y { 0 };
    uint32_t pointer_buttons_down { 0 };

    // Pointer interaction with window chrome (titlebar buttons, titlebar drags
    // and resize borders), mirroring SerenityOS's WindowFrame/Button state.
    Toplevel* pressed_button_toplevel { nullptr };
    int pressed_button_index { -1 };
    Toplevel* last_titlebar_press_toplevel { nullptr };
    uint32_t last_titlebar_press_time { 0 };
    double last_titlebar_press_x { 0 };
    double last_titlebar_press_y { 0 };
    bool double_click_pending { false };

    // The (single) window menu, if any is open.
    WindowMenu window_menu;
};
