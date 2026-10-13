/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
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
#include <wlr/types/wlr_input_device.h> // wlr_input_device* (pointer_devices, R4.6)
}

// wlroots pointer types the compositor model refers to. Defining them here, in
// the compositor's own header, means the policy modules only pull in wlroots
// where they actually use it (D5: keep wlroots behind the platform layer).
struct wlr_allocator;
struct wlr_backend;
struct wlr_cursor;
struct wlr_foreign_toplevel_handle_v1;
struct wlr_foreign_toplevel_manager_v1;
// R4.8 standard interop protocols (ext-workspace-v1, ext-foreign-toplevel-list-v1,
// xdg-activation-v1).
struct wlr_ext_workspace_manager_v1;
struct wlr_ext_workspace_group_handle_v1;
struct wlr_ext_workspace_handle_v1;
struct wlr_ext_foreign_toplevel_list_v1;
struct wlr_ext_foreign_toplevel_handle_v1;
struct wlr_ext_foreign_toplevel_image_capture_source_manager_v1;
struct wlr_xdg_activation_v1;
struct wlr_xdg_toplevel_icon_manager_v1;
struct wlr_xdg_system_bell_v1;
struct wlr_cursor_shape_manager_v1;
struct wlr_idle_inhibit_manager_v1;
struct wlr_idle_inhibitor_v1;
struct wlr_idle_notifier_v1;
struct wlr_keyboard;
struct wlr_layer_shell_v1;
struct wlr_layer_surface_v1;
struct wlr_output;
struct wlr_output_layout;
struct wlr_output_manager_v1;
struct wlr_output_power_manager_v1;
struct wlr_renderer;
struct wlr_relative_pointer_manager_v1;
struct wlr_keyboard_shortcuts_inhibit_manager_v1;
struct wlr_compositor;
struct wlr_pointer;
struct wlr_pointer_gestures_v1;
struct wlr_switch;
struct wlr_xwayland;
struct wlr_xwayland_surface;
struct wlr_tablet;
struct wlr_tablet_tool;
struct wlr_tablet_pad;
struct wlr_tablet_manager_v2;
struct wlr_tablet_v2_tablet;
struct wlr_tablet_v2_tablet_tool;
struct wlr_tablet_v2_tablet_pad;
struct wlr_pointer_constraints_v1;
struct wlr_pointer_constraint_v1;
struct wlr_scene;
struct wlr_scene_buffer;
struct wlr_scene_layer_surface_v1;
struct wlr_scene_output_layout;
struct wlr_scene_node;
struct wlr_scene_surface;
struct wlr_scene_tree;
struct wlr_seat;
struct wlr_drag;
struct wlr_session_lock_manager_v1;
struct wlr_session_lock_surface_v1;
struct wlr_session_lock_v1;
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
    // Re-configure a session lock surface on output size/scale change (R4.1).
    wl_listener commit { };
};

// The inputs that determine a server-side decoration frame's pixels. Cached on
// the toplevel so a client's content commit only re-renders (bitmap alloc,
// theme gradient titlebar, buffer upload) when the decoration actually changed.
struct FrameCache {
    int content_width { 0 };
    int content_height { 0 };
    bool active { true };
    bool maximized { false };
    bool rolled_up { false };
    bool highlighted { false };
    int hovered_button { -1 };
    int pressed_button { -1 };
    Gfx::Bitmap const* icon { nullptr };
    int scale { 1 };
    ByteString title;
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
    // The window icon, shown by the window switcher (bottom-right of the
    // thumbnail) and by frame/taskbar chrome. Unset for plain Wayland clients
    // (SerenityOS clients supply one over the WindowServer protocol).
    RefPtr<Gfx::Bitmap> icon;
    Vector<Gfx::IntRect> button_rects;
    // The inputs of the last rendered frame, if any (see FrameCache).
    Optional<FrameCache> frame_cache;
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
    // The Taskbar button rect for this window (reported over the
    // WindowManagerServer), so minimize/restore can animate the outline to it.
    Gfx::IntRect taskbar_rect;
    // The decoration object, if any; the server-side mode is applied once the
    // surface is initialized (set_mode schedules a configure).
    wlr_xdg_toplevel_decoration_v1* decoration { nullptr };
    // True when the client draws its own decorations. This is the default for
    // Wayland clients (Firefox/GTK never ask for server-side decorations); a
    // client that explicitly requests server-side decorations (the LibGUI/
    // Serenity apps do) gets the compositor frame instead.
    bool client_side_decorations { true };
    // Whether we have already sent a decoration configure. set_mode() schedules
    // a configure, so it must run only when the mode actually changes — not on
    // every commit (that would configure-storm the client).
    bool decoration_mode_applied { false };
    bool menubar_visible { true };
    // The window stack (workspace grid cell) this window belongs to. WS1.
    u32 workspace_row { 0 };
    u32 workspace_column { 0 };
    i32 wm_id { 0 };
    // xdg-session-management-v1 (R4.2): set when this toplevel belongs to a
    // session, so the compositor can store/restore its geometry.
    bool session_managed { false };
    ByteString session_id;
    ByteString session_name;
    Gfx::IntPoint session_restore_position;
    bool has_session_restore_position { false };
    // Applied at the initial commit (restore_toplevel runs before the toplevel
    // is initialized, where set_size would assert).
    Gfx::IntSize session_restore_size;
    bool has_session_restore_size { false };
    // Monotonic focus sequence, so the window switcher can list windows
    // most-recently-focused first (mirroring Serenity's window stack). WS3.
    u64 focus_order { 0 };
    wlr_foreign_toplevel_handle_v1* foreign_handle { nullptr };
    // R4.8: the ext-foreign-toplevel-list handle (title/app_id), if exposed.
    wlr_ext_foreign_toplevel_handle_v1* ext_foreign_handle { nullptr };
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
    // True for applets ("serenity-applet[:<name>]"): the compositor lays these
    // out in the Taskbar's applet area (WS4, AppletManager). applet_name is the
    // suffix, matched against [Applet] Order.
    bool is_applet { false };
    ByteString applet_name;
    // The client's serenity_toplevel chrome handle, if it requested one. Used to
    // report the applet's real output position (the applet area moves it away
    // from its anchored corner), so widget/screen-relative coordinates (tooltips)
    // are exact.
    wl_resource* chrome { nullptr };
    // The arrange-relevant committed state from the last commit that triggered a
    // layer re-arrange. A content-only commit (a new buffer, damage) does not
    // change these, so arrange_layers is skipped and every layer surface is not
    // re-configured — otherwise a client that commits in response to a configure
    // spins (the "configure storm"; see docs/04-known-gaps.md).
    struct ArrangeState {
        bool valid { false };
        uint32_t desired_width { 0 };
        uint32_t desired_height { 0 };
        // The committed (configured) size. The applet layout reads it, so a
        // change must re-arrange even when the desired size did not change (a
        // client can size itself between the first commit and the compositor's
        // first arrange; e.g. the Audio applet resizes after show()).
        uint32_t width { 0 };
        uint32_t height { 0 };
        uint32_t anchor { 0 };
        int32_t exclusive_zone { 0 };
        int32_t margin_top { 0 };
        int32_t margin_right { 0 };
        int32_t margin_bottom { 0 };
        int32_t margin_left { 0 };
        uint32_t layer { 0 };
    };
    ArrangeState arrange_state;
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

// A pointer device plus its gesture listeners (touchpad swipe/pinch/hold), which
// are forwarded to clients as wp-pointer-gestures-v1.
struct Pointer {
    Server* server { nullptr };
    wlr_input_device* device { nullptr };
    wlr_pointer* pointer { nullptr };
    wl_listener swipe_begin { };
    wl_listener swipe_update { };
    wl_listener swipe_end { };
    wl_listener pinch_begin { };
    wl_listener pinch_update { };
    wl_listener pinch_end { };
    wl_listener hold_begin { };
    wl_listener hold_end { };
    wl_listener destroy { };
};

// An X11 (Xwayland) window. X11 clients draw their own decorations, so the
// compositor only manages content, position, focus and stacking (no SSD frame).
struct X11Toplevel {
    Server* server { nullptr };
    wlr_xwayland_surface* xsurface { nullptr };
    wlr_scene_tree* scene_tree { nullptr };
    wlr_scene_surface* scene { nullptr };
    wlr_scene_buffer* frame_buffer { nullptr };
    bool mapped { false };
    bool override_redirect { false };
    // Server-side decoration frame (X11 clients that don't draw their own).
    bool frame_decorated { false };
    int frame_left { 0 };
    int frame_top { 0 };
    int frame_right { 0 };
    int frame_bottom { 0 };
    Gfx::IntSize frame_size;
    Vector<Gfx::IntRect> button_rects;
    Gfx::IntRect titlebar_icon_rect;
    bool frame_active { false };
    bool maximized { false };
    Gfx::IntSize restore_size;
    // The WindowManagerServer id, so the Taskbar can list/activate/minimize it.
    i32 wm_id { 0 };
    // The window title (kept here so the WM module needs no wlroots types).
    ByteString title;
    // WM_CLASS (res_name/instance) and res_class, for app identity/grouping.
    ByteString wm_instance;
    ByteString wm_class;
    // The window icon, fetched from _NET_WM_ICON (shown by the Taskbar button and
    // the Alt-Tab switcher), like Toplevel::icon for xdg windows.
    RefPtr<Gfx::Bitmap> icon;
    // _NET_WM_STATE hints and transiency (docs/20-xwayland-next-steps.md item 7).
    bool skip_taskbar { false };
    bool always_on_top { false };
    bool demands_attention { false };
    bool modal { false };
    X11Toplevel* transient_parent { nullptr };
    // Monotonic focus sequence (like Toplevel::focus_order), so the Alt-Tab
    // switcher can list X11 and xdg windows together most-recently-focused first.
    u64 focus_order { 0 };
    // The workspace stack this window belongs to (like Toplevel::workspace_*):
    // X11 windows join the current stack when they map and hide/show with it.
    u32 workspace_row { 0 };
    u32 workspace_column { 0 };
    int hovered_button { -1 };
    int pressed_button { -1 };
    wl_listener associate { };
    wl_listener dissociate { };
    wl_listener surface_map { };
    wl_listener surface_unmap { };
    wl_listener destroy { };
    wl_listener request_configure { };
    wl_listener request_close { };
    wl_listener request_activate { };
    wl_listener set_title { };
    wl_listener set_class { };
    wl_listener set_icon { };
    wl_listener set_hints { };
    wl_listener request_skip_taskbar { };
    wl_listener request_demands_attention { };
};

// A reference to a window: it is either an xdg toplevel or an X11 (Xwayland)
// window. The window-manager policy paths that must treat both kinds alike (the
// Alt-Tab switcher, and later workspace visibility and window icons) go through
// this so the xdg-only ones stay simple. See docs/20-xwayland-next-steps.md.
// The accessors (title/rect/focus/...) live next to their consumers, since they
// read wlroots types this header only forward-declares.
struct WindowRef {
    Toplevel* xdg { nullptr };
    X11Toplevel* x11 { nullptr };

    bool is_valid() const { return xdg != nullptr || x11 != nullptr; }
    bool is_x11() const { return x11 != nullptr; }
};

// A switch device (lid, tablet-mode, ...).
struct SwitchDevice {
    Server* server { nullptr };
    wlr_switch* device { nullptr };
    wl_listener toggle { };
    wl_listener destroy { };
};

// A tablet pad (express keys + ring/strip), forwarded as zwp_tablet_pad_v2.
struct TabletPad {
    Server* server { nullptr };
    wlr_tablet_pad* pad { nullptr };
    wlr_tablet_v2_tablet_pad* v2_pad { nullptr };
    wl_listener button { };
    wl_listener ring { };
    wl_listener strip { };
    wl_listener destroy { };
};

struct Decoration {
    Server* server { nullptr };
    Toplevel* toplevel { nullptr };
    wlr_xdg_toplevel_decoration_v1* decoration { nullptr };
    wl_listener request_mode { };
    wl_listener destroy { };
};

// A client's idle inhibitor (zwp_idle_inhibit_v1): while any is present the seat
// is never considered idle. R4.1.
struct IdleInhibitor {
    Server* server { nullptr };
    wlr_idle_inhibitor_v1* inhibitor { nullptr };
    wl_listener destroy { };
};

// One output's surface in an ext-session-lock-v1 lock. R4.1.
struct LockSurface {
    Server* server { nullptr };
    wlr_session_lock_surface_v1* lock_surface { nullptr };
    wlr_scene_surface* scene { nullptr };
    bool configured { false };
    Gfx::IntSize configured_size;
    wl_listener commit { };
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
    // R4.8: the standard interop surface. ext-workspace-v1 maps the SDE grid,
    // ext-foreign-toplevel-list-v1 maps the window list, xdg-activation-v1 lets
    // clients ask to be focused.
    wlr_ext_workspace_manager_v1* ext_workspace_manager { nullptr };
    wlr_ext_workspace_group_handle_v1* ext_workspace_group { nullptr };
    Vector<wlr_ext_workspace_handle_v1*> ext_workspaces;
    wlr_ext_foreign_toplevel_list_v1* ext_foreign_toplevel_list { nullptr };
    wlr_ext_foreign_toplevel_image_capture_source_manager_v1* ext_toplevel_capture_source_manager { nullptr };
    wlr_xdg_activation_v1* xdg_activation { nullptr };
    // R5 protocol parity (docs/21): client window icons and the system bell.
    wlr_xdg_toplevel_icon_manager_v1* toplevel_icon_manager { nullptr };
    wl_listener toplevel_icon_set { };
    wlr_xdg_system_bell_v1* system_bell { nullptr };
    wl_listener system_bell_ring { };
    wlr_cursor_shape_manager_v1* cursor_shape_manager { nullptr };
    wlr_output_manager_v1* output_manager { nullptr };
    // R5: native-app compatibility protocols (mostly wlroots-implemented; these
    // need a little compositor glue — relative motion and shortcut inhibition).
    wlr_relative_pointer_manager_v1* relative_pointer_manager { nullptr };
    wlr_keyboard_shortcuts_inhibit_manager_v1* keyboard_shortcuts_inhibit { nullptr };
    // zwp-pointer-constraints-v1: pointer lock/confine for games. The active
    // constraint (for the focused pointer surface) is enforced in
    // process_cursor_motion(); null when none applies.
    wlr_pointer_constraints_v1* pointer_constraints { nullptr };
    wlr_pointer_constraint_v1* active_pointer_constraint { nullptr };
    wl_listener new_pointer_constraint { };
    // Explicit drag-and-drop (data-device DnD between windows): the active drag,
    // the surface it is currently over, and the scene node drawing its icon.
    wlr_drag* active_drag { nullptr };
    wlr_surface* drag_target { nullptr };
    wlr_scene_tree* drag_icon_tree { nullptr };
    wlr_scene_node* drag_icon_node { nullptr };
    wl_listener seat_request_start_drag { };
    wl_listener seat_start_drag { };
    wl_listener drag_destroy { };
    wl_listener drag_icon_destroy { };
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
    wl_listener touch_down { };
    wl_listener touch_motion { };
    wl_listener touch_up { };
    wl_listener touch_cancel { };
    wl_listener seat_request_cursor { };
    wl_listener seat_pointer_focus_change { };
    wl_listener seat_request_set_selection { };
    wl_listener seat_request_set_primary_selection { };
    wl_listener new_virtual_keyboard { };
    wl_listener new_virtual_pointer { };
    wl_listener ext_workspace_commit { };
    wl_listener xdg_activation_activate { };
    wl_listener cursor_shape_request { };

    Vector<NonnullOwnPtr<Output>> outputs;
    Vector<NonnullOwnPtr<Toplevel>> toplevels;
    Vector<NonnullOwnPtr<Popup>> popups;
    Vector<NonnullOwnPtr<LayerSurface>> layer_surfaces;
    // The on_demand layer surface that currently holds keyboard focus (a client
    // popup such as the Wi-Fi network list or its passphrase field), if any, so
    // focus can be restored when it unmaps.
    LayerSurface* keyboard_focus_layer { nullptr };
    Vector<NonnullOwnPtr<Keyboard>> keyboards;
    // Pointer devices, so input.ini's libinput settings can be (re)applied when
    // the config changes. See R4.6.
    Vector<wlr_input_device*> pointer_devices;
    Vector<wlr_input_device*> touch_devices;
    // Live virtual touch devices (serenity-virtual-input); the seat advertises
    // the touch capability while any exists.
    int virtual_touch_count { 0 };
    Vector<NonnullOwnPtr<Pointer>> pointer_objects;
    Vector<NonnullOwnPtr<SwitchDevice>> switch_objects;
    // wp-pointer-gestures-v1: touchpad swipe/pinch/hold.
    wlr_pointer_gestures_v1* pointer_gestures { nullptr };
    // XWayland (R5): X11 client support.
    wlr_compositor* compositor { nullptr };
    wlr_xwayland* xwayland { nullptr };
    // Set when the X server is up (the xwayland "ready" event), so the compositor
    // only calls XWM-backed APIs (e.g. set_workareas) once xwm exists.
    bool xwayland_started { false };
    Vector<NonnullOwnPtr<X11Toplevel>> x11_toplevels;
    X11Toplevel* focused_x11 { nullptr };
    wl_listener new_xwayland_surface { };
    wl_listener xwayland_ready { };
    // zwp-tablet-v2 (pen/tablet): the manager plus the v2 tablet/tool wrappers.
    wlr_tablet_manager_v2* tablet_manager { nullptr };
    HashMap<u64, wlr_tablet_v2_tablet*> tablet_map;
    HashMap<u64, wlr_tablet_v2_tablet_tool*> tablet_tool_map;
    Vector<NonnullOwnPtr<TabletPad>> tablet_pads;
    wl_listener tablet_tool_axis { };
    wl_listener tablet_tool_proximity { };
    wl_listener tablet_tool_tip { };
    wl_listener tablet_tool_button { };
    Vector<NonnullOwnPtr<Decoration>> decorations;
    // serenity_window_manager resources, so the compositor can ask a client to
    // dismiss its menus (see maybe_dismiss_layer_menus).
    Vector<wl_resource*> serenity_window_managers;

    CursorMode cursor_mode { CursorMode::Passthrough };
    Toplevel* grabbed_toplevel { nullptr };
    X11Toplevel* grabbed_x11 { nullptr };
    // The X11 window whose frame button is held, and which one.
    X11Toplevel* pressed_button_x11 { nullptr };
    int pressed_button_x11_index { -1 };
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

    // --- Session safety net (R4.1), see docs/11-r4-implementation-plan.md ---
    // Idle notification (ext-idle-notify-v1), idle inhibition
    // (zwp_idle_inhibit_v1), DPMS (wlr-output-power-management) and session lock
    // (ext-session-lock-v1).
    wlr_idle_notifier_v1* idle_notifier { nullptr };
    wlr_idle_inhibit_manager_v1* idle_inhibit_manager { nullptr };
    wlr_output_power_manager_v1* output_power_manager { nullptr };
    wlr_session_lock_manager_v1* session_lock_manager { nullptr };
    wlr_session_lock_v1* session_lock { nullptr };
    bool locked { false };
    Vector<NonnullOwnPtr<IdleInhibitor>> idle_inhibitors;
    Vector<NonnullOwnPtr<LockSurface>> lock_surfaces;
    wl_listener new_idle_inhibitor { };
    wl_listener output_power_set_mode { };
    wl_listener session_lock_new_lock { };
    wl_listener session_lock_new_surface { };
    wl_listener session_lock_unlock { };
    wl_listener session_lock_destroy { };
};
