/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowManager.h"

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibGfx/Painter.h>
#include <LibGfx/Palette.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/Connection.h>
#include <WindowServer/WindowManagerClientEndpoint.h>
#include <WindowServer/WindowManagerServerEndpoint.h>
#include <WindowServer/WindowType.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "AppletManager.h"
#include "Config.h"
#include <WindowServer/SystemEffects.h>
#include "Overlays.h"
#include "WindowManagerServerDefaultStub.h"

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
}

namespace WindowManager {

static HostHooks g_hooks;

void set_host_hooks(HostHooks hooks)
{
    g_hooks = hooks;
}

void set_toplevel_minimized(Server& server, Toplevel& toplevel, bool minimized)
{
    // A blocking modal dialog cannot be minimized (SerenityOS Window menubar
    // keeps minimize disabled for modal windows).
    if (minimized && toplevel.modal)
        return;
    if (g_hooks.set_minimized)
        g_hooks.set_minimized(server, toplevel, minimized);
    else {
        toplevel.minimized = minimized;
        update_toplevel_visibility(toplevel);
    }
}

// The compositor's end of the Serenity WindowManagerServer protocol.
class WindowManagerServerConnection final
    : public IPC::Connection<WindowManagerServerEndpoint, WindowManagerClientEndpoint>
    , public ::SDE::WindowManagerServerDefaultStub
    , public WindowManagerClientEndpoint::Proxy<WindowManagerServerEndpoint> {
public:
    static NonnullRefPtr<WindowManagerServerConnection> create(Server& server, NonnullOwnPtr<Core::LocalSocket> socket)
    {
        return adopt_ref(*new WindowManagerServerConnection(server, move(socket)));
    }

    i32 manager_window_id() const { return m_manager_window_id; }

    virtual void set_event_mask(u32 event_mask) override { m_event_mask = event_mask; }

    virtual void set_manager_window(i32 window_id) override
    {
        m_manager_window_id = window_id;
        for (auto& toplevel : m_server.toplevels) {
            emit_window_state(*toplevel);
            emit_window_icon(*toplevel);
        }
        for (auto& toplevel : m_server.x11_toplevels)
            emit_window_state(*toplevel);
        // If the applets already mapped, this WM missed the area-size-changed
        // events; tell it the current size so it reports the area position (and
        // the applets are placed on the Taskbar rather than at the fallback).
        async_applet_area_size_changed(window_id, AppletManager::area_size());
        // Tell the Taskbar whether to group a button per application.
        async_taskbar_group_windows(window_id, Config::taskbar().group_windows);
    }

    virtual void set_active_window(i32, i32 window_id) override
    {
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            if (toplevel->minimized)
                set_toplevel_minimized(m_server, *toplevel, false);
            if (g_hooks.focus_toplevel)
                g_hooks.focus_toplevel(m_server, toplevel);
            emit_window_state(*toplevel);
        } else if (auto* x11 = find_x11_by_wm_id(m_server, window_id)) {
            if (g_hooks.focus_x11)
                g_hooks.focus_x11(m_server, x11);
            emit_window_state(*x11);
        }
    }

    virtual void set_window_minimized(i32, i32 window_id, bool minimized) override
    {
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            set_toplevel_minimized(m_server, *toplevel, minimized);
            emit_window_state(*toplevel);
        } else if (auto* x11 = find_x11_by_wm_id(m_server, window_id)) {
            if (g_hooks.set_minimized_x11)
                g_hooks.set_minimized_x11(m_server, *x11, minimized);
            emit_window_state(*x11);
        }
    }

    virtual void toggle_show_desktop() override
    {
        auto& server = m_server;
        server.show_desktop = !server.show_desktop;
        for (auto& toplevel : server.toplevels) {
            bool changed = false;
            if (server.show_desktop) {
                if (!toplevel->minimized) {
                    toplevel->hidden_by_show_desktop = true;
                    set_toplevel_minimized(server, *toplevel, true);
                    changed = true;
                }
            } else if (toplevel->hidden_by_show_desktop) {
                toplevel->hidden_by_show_desktop = false;
                set_toplevel_minimized(server, *toplevel, false);
                changed = true;
            }
            if (changed)
                emit_window_state(*toplevel);
        }
    }
    virtual void start_window_resize(i32, i32 window_id, i32 resize_direction) override
    {
        // The Taskbar asks the compositor to begin resizing a window.
        auto* toplevel = find_toplevel_by_wm_id(m_server, window_id);
        if (!toplevel)
            return;
        if (g_hooks.begin_interactive)
            g_hooks.begin_interactive(*toplevel, CursorMode::Resize, static_cast<uint32_t>(resize_direction));
    }
    virtual void popup_window_menu(i32, i32 window_id, Gfx::IntPoint screen_position) override
    {
        // Right-clicking a Taskbar window-list button opens the window menu. The
        // compositor already renders that menu for the titlebar, so reuse it (the
        // layout clamps it on-screen, which places it above the Taskbar).
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            if (g_hooks.open_window_menu)
                g_hooks.open_window_menu(m_server, *toplevel, screen_position);
        }
    }
    // The Taskbar reports each button's rect, so minimize/restore can animate the
    // SerenityOS outline between the window and its button.
    virtual void set_window_taskbar_rect(i32, i32 window_id, Gfx::IntRect const& rect) override
    {
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id))
            toplevel->taskbar_rect = rect;
    }

    // Applets are layer surfaces; the compositor owns their layout (WS4). The
    // Taskbar tells us where the applet area sits and we place each applet.
    virtual void set_applet_area_position(Gfx::IntPoint position) override
    {
        AppletManager::set_position(m_server, position);
    }

    virtual void set_workspace(u32 row, u32 column) override
    {
        switch_workspace(m_server, row, column, false);
    }
    virtual void set_keymap(ByteString const&) override { }

private:
    explicit WindowManagerServerConnection(Server& server, NonnullOwnPtr<Core::LocalSocket> socket)
        : IPC::Connection<WindowManagerServerEndpoint, WindowManagerClientEndpoint>(*this, move(socket))
        , WindowManagerClientEndpoint::Proxy<WindowManagerServerEndpoint>(*this, { })
        , m_server(server)
    {
    }

    Server& m_server;
    i32 m_manager_window_id { -1 };
    u32 m_event_mask { 0 };
};

static Vector<NonnullRefPtr<WindowManagerServerConnection>> g_connections;

void accept_connection(Server& server, NonnullOwnPtr<Core::LocalSocket> socket)
{
    g_connections.append(WindowManagerServerConnection::create(server, move(socket)));
}

Toplevel* find_toplevel_by_wm_id(Server& server, i32 wm_id)
{
    for (auto& toplevel : server.toplevels) {
        if (toplevel->wm_id == wm_id)
            return toplevel.ptr();
    }
    return nullptr;
}

// --- snapping (WS2) ---------------------------------------------------------

static Toplevel* focused_toplevel(Server& server)
{
    auto* surface = server.seat->keyboard_state.focused_surface;
    if (!surface)
        return nullptr;
    auto* xdg_toplevel = wlr_xdg_toplevel_try_from_wlr_surface(surface);
    if (!xdg_toplevel)
        return nullptr;
    auto* content_tree = static_cast<wlr_scene_tree*>(xdg_toplevel->base->data);
    if (!content_tree)
        return nullptr;
    auto* frame_tree = content_tree->node.parent;
    if (!frame_tree)
        return nullptr;
    return static_cast<Toplevel*>(frame_tree->node.data);
}

static Gfx::IntRect usable_area(Server& server)
{
    if (g_hooks.primary_usable_area)
        return g_hooks.primary_usable_area(server);
    return {};
}

// The xdg_toplevel tiled edges for a tile type.
static uint32_t tile_edges(WindowTileType type)
{
    switch (type) {
    case WindowTileType::Left:
        return WLR_EDGE_LEFT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    case WindowTileType::Right:
        return WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    case WindowTileType::Top:
        return WLR_EDGE_TOP | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    case WindowTileType::Bottom:
        return WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    case WindowTileType::HorizontallyMaximized:
        return WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    case WindowTileType::VerticallyMaximized:
        return WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
    case WindowTileType::Maximized:
        return WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
    default:
        return 0;
    }
}

static void apply_tile_geometry(Toplevel& toplevel, Gfx::IntRect const& rect, uint32_t xdg_edges)
{
    wlr_xdg_toplevel_set_tiled(toplevel.xdg_toplevel, xdg_edges);
    wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, toplevel.tile_type == WindowTileType::Maximized);
    wlr_scene_node_set_position(&toplevel.scene_tree->node, rect.x(), rect.y());
    if (g_hooks.report_position)
        g_hooks.report_position(toplevel);
    wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel,
        max(1, rect.width() - toplevel.frame_left - toplevel.frame_right),
        max(1, rect.height() - toplevel.frame_top - toplevel.frame_bottom));
}

static void set_untiled(Toplevel& toplevel)
{
    toplevel.tile_type = WindowTileType::None;
    toplevel.maximized = false;
    wlr_xdg_toplevel_set_tiled(toplevel.xdg_toplevel, 0);
    wlr_xdg_toplevel_set_maximized(toplevel.xdg_toplevel, false);
    wlr_scene_node_set_position(&toplevel.scene_tree->node, toplevel.restore_position.x(), toplevel.restore_position.y());
    if (g_hooks.report_position)
        g_hooks.report_position(toplevel);
    wlr_xdg_toplevel_set_size(toplevel.xdg_toplevel, toplevel.restore_size.width(), toplevel.restore_size.height());
}

static void set_tiled(Server& server, Toplevel& toplevel, WindowTileType type)
{
    // Remember where to restore to on the first tile.
    if (toplevel.tile_type == WindowTileType::None) {
        toplevel.restore_size = { toplevel.xdg_toplevel->base->geometry.width, toplevel.xdg_toplevel->base->geometry.height };
        toplevel.restore_position = { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y };
    }

    auto area = usable_area(server);
    if (area.is_empty())
        return;

    auto rect = WindowServer::tile_rect(area, type);
    uint32_t const xdg_edges = tile_edges(type);

    toplevel.tile_type = type;
    toplevel.maximized = type == WindowTileType::Maximized;
    apply_tile_geometry(toplevel, rect, xdg_edges);
    emit_window_state(toplevel);
    dbgln("WindowServer: tiled wm_id={} as {}", toplevel.wm_id, to_underlying(type));
}

static void toggle_tile(Server& server, Toplevel& toplevel, WindowTileType type)
{
    if (toplevel.tile_type == type)
        set_untiled(toplevel);
    else
        set_tiled(server, toplevel, type);
}

static bool tile_focused(Server& server, WindowTileType type)
{
    auto* toplevel = focused_toplevel(server);
    if (!toplevel)
        return false;
    toggle_tile(server, *toplevel, type);
    return true;
}

static bool maximize_focused(Server& server)
{
    auto* toplevel = focused_toplevel(server);
    if (!toplevel)
        return false;
    if (toplevel->maximized)
        set_untiled(*toplevel);
    else
        set_tiled(server, *toplevel, WindowTileType::Maximized);
    return true;
}

static bool minimize_focused(Server& server)
{
    auto* toplevel = focused_toplevel(server);
    if (!toplevel)
        return false;
    set_toplevel_minimized(server, *toplevel, true);
    emit_window_state(*toplevel);
    return true;
}

// --- drag-to-edge snapping --------------------------------------------------

static OwnPtr<ChromeNode> s_tile_preview;
static WindowTileType s_pending_tile { WindowTileType::None };

// --- geometry HUD (WS2) -----------------------------------------------------
//
// SerenityOS WindowGeometryOverlay: a small size readout shown while moving or
// resizing. During a move that is previewing a tile it shows the prospective tile
// size instead of the window's.

static OwnPtr<ChromeNode> s_geometry_hud;

void hide_geometry_hud()
{
    if (s_geometry_hud)
        s_geometry_hud->set_visible(false);
}

static void show_geometry_hud(Server& server, Gfx::IntRect geometry, bool resizing)
{
    // [Effects] WindowGeometry=false disables the HUD entirely; ShowGeometry
    // selects whether it appears while moving, resizing, both or never (R4.7).
    auto const& effects = Config::effects();
    if (!effects.window_geometry) {
        hide_geometry_hud();
        return;
    }
    auto const show = WindowServer::ShowGeometryTools::string_to_enum(effects.show_geometry);
    if (show == WindowServer::ShowGeometry::Never
        || (show == WindowServer::ShowGeometry::OnMoveOnly && resizing)
        || (show == WindowServer::ShowGeometry::OnResizeOnly && !resizing)) {
        hide_geometry_hud();
        return;
    }
    if (!server.overlay_tree || geometry.is_empty())
        return;

    auto label = ByteString::formatted("{}x{}", geometry.width(), geometry.height());
    auto const& font = Gfx::FontDatabase::default_font();
    int const pad_x = 10;
    int const pad_y = 4;
    Gfx::IntSize const hud_size { font.width_rounded_up(label) + pad_x * 2, font.pixel_size_rounded_up() + pad_y * 2 };
    Gfx::IntRect hud_rect { {}, hud_size };
    hud_rect.center_within(geometry);

    // Keep the HUD on the screen.
    auto const area = usable_area(server);
    if (!area.is_empty()) {
        if (hud_rect.left() < area.left())
            hud_rect.set_x(area.x());
        if (hud_rect.top() < area.top())
            hud_rect.set_y(area.y());
        if (hud_rect.right() > area.right())
            hud_rect.set_right_without_resize(area.right());
        if (hud_rect.bottom() > area.bottom())
            hud_rect.set_bottom_without_resize(area.bottom());
    }

    if (!s_geometry_hud)
        s_geometry_hud = make<ChromeNode>(*server.overlay_tree);
    s_geometry_hud->set_rect(hud_rect);

    auto const* palette = g_hooks.palette ? &g_hooks.palette(server) : nullptr;
    s_geometry_hud->render([&](Gfx::Painter& painter) {
        Gfx::IntRect local { {}, hud_size };
        Gfx::Color background = Gfx::Color::from_rgb(0x2b2b2b).with_alpha(0xe0);
        Gfx::Color border = Gfx::Color::from_rgb(0x9db8d2);
        Gfx::Color text = Gfx::Color::White;
        if (palette) {
            background = palette->overlay_background();
            border = palette->overlay_text().with_alpha(0x80);
            text = palette->overlay_text();
        }
        painter.fill_rect(local, background);
        painter.draw_rect(local.shrunken(1, 1), border);
        painter.draw_ui_text(local, label, font, Gfx::TextAlignment::Center, text);
    });
    s_geometry_hud->set_visible(true);
    s_geometry_hud->raise_to_top();
}

void update_resize_hud(Server& server, Toplevel& toplevel, Gfx::IntRect content_rect)
{
    // Show the frame rect (content plus the server-side frame), like SerenityOS.
    Gfx::IntRect geometry {
        content_rect.x() - toplevel.frame_left,
        content_rect.y() - toplevel.frame_top,
        content_rect.width() + toplevel.frame_left + toplevel.frame_right,
        content_rect.height() + toplevel.frame_top + toplevel.frame_bottom,
    };
    show_geometry_hud(server, geometry, true);
}

static Gfx::IntRect toplevel_frame_rect(Toplevel const& toplevel)
{
    return { toplevel.scene_tree->node.x, toplevel.scene_tree->node.y, toplevel.frame_size.width(), toplevel.frame_size.height() };
}

void update_move_snap(Server& server, Toplevel& toplevel)
{
    // [Tiling] DragToEdge=false disables edge snapping entirely (WS2).
    if (!Config::tiling().drag_to_edge) {
        if (s_tile_preview)
            s_tile_preview->set_visible(false);
        s_pending_tile = WindowTileType::None;
        hide_geometry_hud();
        return;
    }

    auto area = usable_area(server);
    if (area.is_empty())
        return;

    int const edge = Config::tiling().edge_threshold;
    double const x = server.cursor->x;
    double const y = server.cursor->y;

    WindowTileType target = WindowTileType::None;
    if (x <= area.x() + edge)
        target = y <= area.y() + edge ? WindowTileType::TopLeft
            : y >= area.bottom() - edge ? WindowTileType::BottomLeft
                                        : WindowTileType::Left;
    else if (x >= area.right() - edge)
        target = y <= area.y() + edge ? WindowTileType::TopRight
            : y >= area.bottom() - edge ? WindowTileType::BottomRight
                                        : WindowTileType::Right;
    else if (y <= area.y() + edge)
        target = WindowTileType::Top;
    else if (y >= area.bottom() - edge)
        target = WindowTileType::Bottom;

    s_pending_tile = target;

    if (target != WindowTileType::None) {
        if (server.overlay_tree) {
            if (!s_tile_preview)
                s_tile_preview = make<ChromeNode>(*server.overlay_tree);
            auto rect = WindowServer::tile_rect(area, target);
            s_tile_preview->set_rect(rect);
            s_tile_preview->render([rect](Gfx::Painter& painter) {
                Gfx::IntRect local { { }, rect.size() };
                painter.fill_rect(local, Gfx::Color::from_rgb(0x3b6ea5).with_alpha(0x50));
                painter.draw_rect(local.shrunken(1, 1), Gfx::Color::from_rgb(0x9db8d2).with_alpha(0xc0));
            });
            s_tile_preview->set_visible(true);
            s_tile_preview->raise_to_top();
        }
    } else if (s_tile_preview) {
        s_tile_preview->set_visible(false);
    }

    // The HUD previews the prospective tile while one is shown, else the window.
    auto geometry = target != WindowTileType::None ? WindowServer::tile_rect(area, target) : toplevel_frame_rect(toplevel);
    show_geometry_hud(server, geometry, false);
}

void finish_move_snap(Server& server, Toplevel& toplevel)
{
    if (s_tile_preview)
        s_tile_preview->set_visible(false);
    hide_geometry_hud();
    if (s_pending_tile != WindowTileType::None)
        set_tiled(server, toplevel, s_pending_tile);
    s_pending_tile = WindowTileType::None;
}

// --- workspaces (WS1) -------------------------------------------------------

static u32 s_rows { 2 };
static u32 s_columns { 2 };
static u32 s_current_row { 0 };
static u32 s_current_column { 0 };

u32 workspace_rows() { return s_rows; }
u32 workspace_columns() { return s_columns; }
u32 current_workspace_row() { return s_current_row; }
u32 current_workspace_column() { return s_current_column; }

void update_toplevel_visibility(Toplevel& toplevel)
{
    // A minimized window is unmapped as far as the stack is concerned; the tile
    // and workspace rules are the same for xdg and X11 windows (WindowRules.h).
    wlr_scene_node_set_enabled(&toplevel.scene_tree->node,
        WindowServer::window_visible_on_workspace(toplevel.workspace_row, toplevel.workspace_column, s_current_row, s_current_column, !toplevel.minimized));
}

void on_toplevel_mapped(Toplevel& toplevel)
{
    toplevel.workspace_row = s_current_row;
    toplevel.workspace_column = s_current_column;
    update_toplevel_visibility(toplevel);
}

void update_x11_toplevel_visibility(X11Toplevel& toplevel)
{
    if (!toplevel.scene_tree)
        return;
    // An X11 window that is withdrawn/minimized is already unmapped (disabled);
    // keep it hidden regardless of the stack. Override-redirect windows (menus,
    // tooltips) are unmanaged/transient: they are never workspace-hid.
    bool const visible = WindowServer::window_visible_on_workspace(toplevel.workspace_row, toplevel.workspace_column, s_current_row, s_current_column, toplevel.mapped, toplevel.override_redirect);
    wlr_scene_node_set_enabled(&toplevel.scene_tree->node, visible);
    dbgln("WindowServer: X11 window {} workspace {},{} visible={}", toplevel.wm_id, toplevel.workspace_row, toplevel.workspace_column, visible);
}

void on_x11_toplevel_mapped(X11Toplevel& toplevel)
{
    // A newly mapped X11 window joins the current stack (like on_toplevel_mapped).
    toplevel.workspace_row = s_current_row;
    toplevel.workspace_column = s_current_column;
    update_x11_toplevel_visibility(toplevel);
}

static void emit_workspace_changed(Server& server)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_workspace_changed(connection->manager_window_id(), s_current_row, s_current_column);
    }
    (void)server;
}

void load_workspaces(Server& server)
{
    auto const& grid = Config::workspaces();
    s_rows = static_cast<u32>(max(1, grid.rows));
    s_columns = static_cast<u32>(max(1, grid.columns));
    for (auto& toplevel : server.toplevels)
        update_toplevel_visibility(*toplevel);
    for (auto& toplevel : server.x11_toplevels)
        update_x11_toplevel_visibility(*toplevel);
    dbgln("WindowServer: workspaces {}x{}, current {},{}", s_rows, s_columns, s_current_row, s_current_column);
    if (g_hooks.workspace_changed)
        g_hooks.workspace_changed(server);
}

// --- window-stack switch overlay (R3) ---------------------------------------
//
// SerenityOS briefly shows a grid of one cell per window stack (workspace) when
// the stack is switched — or when the user asks for the stack they are already on
// — with the current stack highlighted, so they can see where they are
// (Compositor::create_window_stack_switch_overlay, 500 ms).
static OwnPtr<ChromeNode> s_stack_switch_overlay;
static RefPtr<Core::Timer> s_stack_switch_overlay_timer;
static constexpr WindowServer::StackOverlayMetrics s_stack_overlay_metrics { 40, 30, 8, 16 };
static constexpr int stack_overlay_duration_ms = 500;

static void hide_window_stack_switch_overlay()
{
    if (s_stack_switch_overlay)
        s_stack_switch_overlay->set_visible(false);
}

static void show_window_stack_switch_overlay(Server& server, u32 row, u32 column)
{
    if (!server.overlay_tree)
        return;

    int const columns = static_cast<int>(workspace_columns());
    int const rows = static_cast<int>(workspace_rows());

    Gfx::IntRect area { 0, 0, 1280, 720 };
    if (g_hooks.primary_usable_area)
        area = g_hooks.primary_usable_area(server);
    Gfx::IntRect const frame = WindowServer::stack_overlay_frame(area, rows, columns, s_stack_overlay_metrics);

    if (!s_stack_switch_overlay)
        s_stack_switch_overlay = make<ChromeNode>(*server.overlay_tree);
    s_stack_switch_overlay->set_rect(frame);

    auto const* palette = g_hooks.palette ? &g_hooks.palette(server) : nullptr;
    s_stack_switch_overlay->render([&](Gfx::Painter& painter) {
        Gfx::IntRect local { {}, frame.size() };
        Gfx::Color background = Gfx::Color::from_rgb(0x2b2b2b).with_alpha(0xe0);
        Gfx::Color border = Gfx::Color::from_rgb(0x9db8d2).with_alpha(0x80);
        Gfx::Color active = Gfx::Color::from_rgb(0x3b6ea5);
        Gfx::Color inactive = Gfx::Color::from_rgb(0x555555);
        if (palette) {
            background = palette->overlay_background();
            border = palette->overlay_text().with_alpha(0x80);
            active = palette->selection();
            // SerenityOS picks the inactive cell colour from the overlay text's
            // luminosity, so it reads on both light and dark themes.
            inactive = palette->overlay_text().luminosity() > 128
                ? palette->window().darkened(0.9f)
                : palette->disabled_text_front();
        }
        painter.fill_rect(local, background);
        painter.draw_rect(local.shrunken(1, 1), border);
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < columns; ++x) {
                auto const cell = WindowServer::stack_overlay_cell(rows, columns, y, x, s_stack_overlay_metrics);
                bool const is_current = static_cast<u32>(y) == row && static_cast<u32>(x) == column;
                painter.fill_rect(cell, is_current ? active : inactive);
            }
        }
    });
    s_stack_switch_overlay->set_visible(true);
    s_stack_switch_overlay->raise_to_top();

    if (!s_stack_switch_overlay_timer)
        s_stack_switch_overlay_timer = Core::Timer::create_single_shot(stack_overlay_duration_ms, hide_window_stack_switch_overlay);
    s_stack_switch_overlay_timer->restart(stack_overlay_duration_ms);
    dbgln("WindowServer: window-stack switch overlay for workspace {},{}", row, column);
}

// --- window-stack transition (R3) -------------------------------------------
//
// SerenityOS slides the outgoing window stack out and the incoming one in when
// the stack (workspace) changes (the Compositor's window-stack transition
// animation). SDE's stacks are not separate scene trees, so the toplevels of both
// stacks are offset for the duration; when it ends every position is restored and
// the outgoing stack is hidden.
struct StackTransitionWindow {
    wlr_scene_tree* tree { nullptr };
    Gfx::IntPoint base;
    Gfx::IntPoint from;
    Gfx::IntPoint to;
};
static Vector<StackTransitionWindow> s_stack_transition;
static RefPtr<Core::Timer> s_stack_transition_timer;
static Server* s_stack_transition_server { nullptr };
static i64 s_stack_transition_start_ms { 0 };
static constexpr int stack_transition_duration_ms = 200;

static i64 monotonic_ms_now()
{
    timespec now { };
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<i64>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

static void finish_window_stack_transition()
{
    auto* server = s_stack_transition_server;
    for (auto& entry : s_stack_transition) {
        if (entry.tree)
            wlr_scene_node_set_position(&entry.tree->node, entry.base.x(), entry.base.y());
    }
    s_stack_transition.clear();
    s_stack_transition_server = nullptr;
    if (s_stack_transition_timer)
        s_stack_transition_timer->stop();
    if (!server)
        return;
    // The switch has already happened, so this hides the outgoing stack.
    for (auto& toplevel : server->toplevels)
        update_toplevel_visibility(*toplevel);
    for (auto& toplevel : server->x11_toplevels)
        update_x11_toplevel_visibility(*toplevel);
}

static void tick_window_stack_transition()
{
    if (!s_stack_transition_server || s_stack_transition.is_empty()) {
        finish_window_stack_transition();
        return;
    }
    float const t = clamp(static_cast<float>(monotonic_ms_now() - s_stack_transition_start_ms) / stack_transition_duration_ms, 0.0f, 1.0f);
    float const eased = WindowServer::stack_transition_eased(t);
    for (auto& entry : s_stack_transition) {
        if (entry.tree)
            wlr_scene_node_set_position(&entry.tree->node,
                WindowServer::stack_transition_lerp(entry.base.x() + entry.from.x(), entry.base.x() + entry.to.x(), eased),
                WindowServer::stack_transition_lerp(entry.base.y() + entry.from.y(), entry.base.y() + entry.to.y(), eased));
    }
    if (t >= 1.0f)
        finish_window_stack_transition();
}

// Start a transition to `new_row`/`new_column` from `old_row`/`old_column`.
// Returns false when there is nothing to animate (so the caller applies the
// visibility change immediately).
static bool start_window_stack_transition(Server& server, u32 new_row, u32 new_column, u32 old_row, u32 old_column)
{
    if (!Config::effects().animate_windows)
        return false;
    // The user is impatient: finish the previous transition instantly.
    if (!s_stack_transition.is_empty())
        finish_window_stack_transition();

    Gfx::IntRect area { 0, 0, 1280, 720 };
    if (g_hooks.primary_usable_area)
        area = g_hooks.primary_usable_area(server);

    // SerenityOS's direction convention: moving right slides the old stack left.
    auto const delta = WindowServer::stack_transition_delta(new_row, new_column, old_row, old_column, area.size());
    int const delta_x = delta.x();
    int const delta_y = delta.y();
    if (delta_x == 0 && delta_y == 0)
        return false;

    auto add = [&](wlr_scene_tree* tree, u32 row, u32 column) {
        bool const incoming = row == new_row && column == new_column;
        bool const outgoing = row == old_row && column == old_column;
        if (!incoming && !outgoing)
            return; // other stacks stay hidden
        Gfx::IntPoint base { tree->node.x, tree->node.y };
        wlr_scene_node_set_enabled(&tree->node, true);
        s_stack_transition.append({ tree, base,
            incoming ? Gfx::IntPoint { -delta_x, -delta_y } : Gfx::IntPoint { 0, 0 },
            incoming ? Gfx::IntPoint { 0, 0 } : Gfx::IntPoint { delta_x, delta_y } });
    };

    for (auto& toplevel : server.toplevels) {
        if (toplevel->minimized)
            continue;
        add(toplevel->scene_tree, toplevel->workspace_row, toplevel->workspace_column);
    }
    for (auto& toplevel : server.x11_toplevels) {
        if (!toplevel->scene_tree || !toplevel->mapped || toplevel->override_redirect)
            continue;
        add(toplevel->scene_tree, toplevel->workspace_row, toplevel->workspace_column);
    }

    if (s_stack_transition.is_empty())
        return false;

    s_stack_transition_server = &server;
    s_stack_transition_start_ms = monotonic_ms_now();
    if (!s_stack_transition_timer)
        s_stack_transition_timer = Core::Timer::create_repeating(15, tick_window_stack_transition);
    s_stack_transition_timer->start();
    dbgln("WindowServer: window-stack transition {},{} -> {},{} (delta {},{})", old_row, old_column, new_row, new_column, delta_x, delta_y);
    return true;
}

void switch_workspace(Server& server, u32 row, u32 column, bool carry_focused)
{
    if (row >= s_rows || column >= s_columns)
        return;
    if (row == s_current_row && column == s_current_column) {
        // SerenityOS switches the stack even when the target is the current one,
        // "so that the user knows which stack they're on": it shows the overlay.
        show_window_stack_switch_overlay(server, row, column);
        return;
    }

    // Carry the focused window into the target stack (Ctrl+Shift+Alt+arrow).
    Toplevel* carried = carry_focused ? focused_toplevel(server) : nullptr;

    u32 const previous_row = s_current_row;
    u32 const previous_column = s_current_column;

    s_current_row = row;
    s_current_column = column;
    if (carried) {
        carried->workspace_row = row;
        carried->workspace_column = column;
    }

    // SerenityOS slides the two stacks (R3). The visibility change happens when
    // the transition finishes; without it (or with nothing to animate) apply it
    // immediately.
    if (!start_window_stack_transition(server, row, column, previous_row, previous_column)) {
        for (auto& toplevel : server.toplevels)
            update_toplevel_visibility(*toplevel);
        for (auto& toplevel : server.x11_toplevels)
            update_x11_toplevel_visibility(*toplevel);
    }

    // Focus a visible window on the new stack so the keyboard follows.
    bool focused = false;
    for (auto& toplevel : server.toplevels) {
        if (toplevel->workspace_row == row && toplevel->workspace_column == column && !toplevel->minimized) {
            if (g_hooks.focus_toplevel)
                g_hooks.focus_toplevel(server, toplevel.ptr());
            focused = true;
            break;
        }
    }
    if (!focused) {
        for (auto& toplevel : server.x11_toplevels) {
            if (toplevel->mapped && !toplevel->override_redirect
                && toplevel->workspace_row == row && toplevel->workspace_column == column) {
                if (g_hooks.focus_x11)
                    g_hooks.focus_x11(server, toplevel.ptr());
                break;
            }
        }
    }

    for (auto& toplevel : server.toplevels)
        emit_window_state(*toplevel);
    for (auto& toplevel : server.x11_toplevels)
        emit_window_state(*toplevel);
    emit_workspace_changed(server);
    if (g_hooks.workspace_changed)
        g_hooks.workspace_changed(server);
    show_window_stack_switch_overlay(server, row, column);
    dbgln("WindowServer: switched to workspace {},{}", row, column);
}

static bool switch_direction(Server& server, int dx, int dy, bool carry)
{
    // Clamp rather than ignoring an edge press: switch_workspace() shows the
    // stack-switch overlay for the current workspace too (SerenityOS does the
    // same, so a press at the edge still tells the user where they are).
    int row = clamp(static_cast<int>(s_current_row) + dy, 0, static_cast<int>(s_rows) - 1);
    int column = clamp(static_cast<int>(s_current_column) + dx, 0, static_cast<int>(s_columns) - 1);
    switch_workspace(server, static_cast<u32>(row), static_cast<u32>(column), carry);
    return true;
}

// Super is the Taskbar's "Windows key": tapping it alone opens the Taskbar
// system menu, but holding it must still act as a modifier for Super+<key>
// shortcuts. As in SerenityOS, the menu is opened on Super *keyup* only if Super
// was pressed and released with no other key in between; any other key clears
// the tap. Super+Space / Super+D / Super+<digit> are the exceptions SerenityOS
// forwards to the Taskbar.
static bool s_super_tapped { false };

void clear_super_tap()
{
    s_super_tapped = false;
}

bool handle_super_key(Server& server, u32 keysym, u32 modifiers, bool pressed)
{
    bool const super_held = (modifiers & WLR_MODIFIER_LOGO) != 0;
    bool const is_super_key = keysym == XKB_KEY_Super_L || keysym == XKB_KEY_Super_R;

    auto emit = [&](auto&& call) {
        for (auto& connection : g_connections) {
            if (connection->manager_window_id() >= 0)
                call(*connection, connection->manager_window_id());
        }
    };

    if (!pressed) {
        if (is_super_key) {
            bool const tapped = s_super_tapped;
            s_super_tapped = false;
            // Tap Super alone -> open the Taskbar's system menu.
            if (tapped) {
                dbgln("WindowServer: Super tapped (Taskbar menu)");
                emit([](auto& connection, i32 wm_id) { connection.async_super_key_pressed(wm_id); });
            }
            return true; // consume the Super release
        }
        return false;
    }

    // Press.
    if (is_super_key) {
        s_super_tapped = true;
        return true; // consume the Super press (clients see the modifier state)
    }
    if (!super_held)
        return false;

    if (s_super_tapped) {
        s_super_tapped = false;
        switch (keysym) {
        case XKB_KEY_space:
            emit([](auto& connection, i32 wm_id) { connection.async_super_space_key_pressed(wm_id); });
            return true;
        case XKB_KEY_d:
        case XKB_KEY_D:
            emit([](auto& connection, i32 wm_id) { connection.async_super_d_key_pressed(wm_id); });
            return true;
        default:
            if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
                auto digit = static_cast<u8>(keysym - XKB_KEY_1 + 1);
                emit([digit](auto& connection, i32 wm_id) { connection.async_super_digit_key_pressed(wm_id, digit); });
                return true;
            }
            break;
        }
    }

    // Super + another key: Super is a modifier for a shortcut, not a tap.
    s_super_tapped = false;
    return false;
}

// --- modal chain (SerenityOS WindowMode) ------------------------------------
//
// A blocking window (a modal dialog) preempts interaction with the rest of its
// modal chain: everything except its own transient descendants and sibling
// blockers is blocked (SerenityOS Window::blocking_modal_window()). It is used to
// block input behind a modal, refuse to minimize a blocking window and keep a
// transient above its parent.

static bool is_transient_descendant_of(Toplevel const* window, Toplevel const* ancestor)
{
    for (auto* w = window; w; w = w->transient_parent) {
        if (w == ancestor)
            return true;
    }
    return false;
}

static bool is_x11_transient_descendant_of(X11Toplevel const* window, X11Toplevel const* ancestor)
{
    for (auto* w = window; w; w = w->transient_parent) {
        if (w == ancestor)
            return true;
    }
    return false;
}

static bool blocks(Toplevel const& blocker, Toplevel const& window)
{
    if (!blocker.modal || &blocker == &window || !blocker.mapped || blocker.minimized)
        return false;
    if (blocker.workspace_row != window.workspace_row || blocker.workspace_column != window.workspace_column)
        return false;
    // A dialog's own children, and sibling blockers (modals sharing a parent),
    // are not blocked by it.
    if (is_transient_descendant_of(&window, &blocker))
        return false;
    if (window.modal && window.transient_parent == blocker.transient_parent)
        return false;
    return true;
}

bool is_toplevel_blocked(Server& server, Toplevel const& window)
{
    if (!window.mapped || window.minimized)
        return false;
    for (auto& entry : server.toplevels) {
        if (blocks(*entry, window))
            return true;
    }
    return false;
}

static bool blocks(X11Toplevel const& blocker, X11Toplevel const& window)
{
    // An X11 window is minimized when it is unmapped, so an unmapped blocker is
    // not on screen.
    if (!blocker.modal || &blocker == &window || !blocker.mapped)
        return false;
    if (blocker.workspace_row != window.workspace_row || blocker.workspace_column != window.workspace_column)
        return false;
    if (is_x11_transient_descendant_of(&window, &blocker))
        return false;
    if (window.modal && window.transient_parent == blocker.transient_parent)
        return false;
    return true;
}

bool is_x11_toplevel_blocked(Server& server, X11Toplevel const& window)
{
    // Override-redirect menus/tooltips are transient UI: never blocked.
    if (window.override_redirect)
        return false;
    // A minimized X11 window is unmapped.
    if (!window.mapped)
        return false;
    for (auto& entry : server.x11_toplevels) {
        if (blocks(*entry, window))
            return true;
    }
    return false;
}

void emit_window_state(Toplevel& toplevel)
{
    StringView title;
    if (toplevel.xdg_toplevel->title)
        title = StringView { toplevel.xdg_toplevel->title, strlen(toplevel.xdg_toplevel->title) };
    StringView app_id;
    if (toplevel.xdg_toplevel->app_id)
        app_id = StringView { toplevel.xdg_toplevel->app_id, strlen(toplevel.xdg_toplevel->app_id) };
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() < 0)
            continue;
        // Chrome windows (the panel, applets, the desktop) are layer surfaces,
        // not toplevels, so nothing reported here is chrome.
        connection->async_window_state_changed(connection->manager_window_id(), 0, toplevel.wm_id, toplevel.workspace_row, toplevel.workspace_column,
            toplevel.frame_active && !toplevel.minimized, is_toplevel_blocked(*toplevel.server, toplevel), toplevel.minimized, false,
            static_cast<i32>(WindowServer::WindowType::Normal), ByteString { title },
            Gfx::IntRect { 0, 0, 0, 0 }, Optional<i32> { }, ByteString { app_id });
    }
}

// Tell the WindowManagerServer listeners (the Taskbar) about a window's icon, so
// window-list buttons can draw it (SerenityOS WindowManager::tell_wms_window_icon_changed).
void emit_window_icon(Toplevel& toplevel)
{
    // Never send an invalid bitmap: the Taskbar dereferences it (SerenityOS
    // always has at least the default window icon).
    if (!toplevel.icon)
        return;
    auto bitmap = toplevel.icon->to_shareable_bitmap();
    bool sent = false;
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() < 0)
            continue;
        connection->async_window_icon_bitmap_changed(connection->manager_window_id(), 0, toplevel.wm_id, bitmap);
        sent = true;
    }
    if (sent)
        dbgln("WindowServer: WM: window {} icon {}x{}", toplevel.wm_id,
            toplevel.icon ? toplevel.icon->width() : 0, toplevel.icon ? toplevel.icon->height() : 0);
}

void emit_window_removed(Toplevel& toplevel)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_window_removed(connection->manager_window_id(), 0, toplevel.wm_id);
    }
}

void emit_window_state(X11Toplevel& toplevel)
{
    dbgln("WindowServer: WM: X11 window {} '{}' (active={}, minimized={})", toplevel.wm_id, toplevel.title, toplevel.frame_active, !toplevel.mapped);
    bool minimized = !toplevel.mapped;
    // An X11 client is identified by its WM_CLASS (the "class" part is the app
    // name), with the instance as a fallback; this groups its taskbar button.
    ByteString app_id = toplevel.wm_class;
    if (app_id.is_empty())
        app_id = toplevel.wm_instance;
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() < 0)
            continue;
        connection->async_window_state_changed(connection->manager_window_id(), 0, toplevel.wm_id, 0, 0,
            toplevel.frame_active && !minimized, is_x11_toplevel_blocked(*toplevel.server, toplevel), minimized, false,
            static_cast<i32>(WindowServer::WindowType::Normal), toplevel.title,
            Gfx::IntRect { 0, 0, 0, 0 }, Optional<i32> { }, app_id);
    }
    // The Taskbar drops an icon that arrives before the window button exists, and the
    // X11 icon is fetched before the window is reported, so re-send it now.
    if (toplevel.icon)
        emit_window_icon(toplevel);
}

void emit_window_removed(X11Toplevel& toplevel)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_window_removed(connection->manager_window_id(), 0, toplevel.wm_id);
    }
}

void emit_window_icon(X11Toplevel& toplevel)
{
    if (!toplevel.icon)
        return;
    auto bitmap = toplevel.icon->to_shareable_bitmap();
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() < 0)
            continue;
        connection->async_window_icon_bitmap_changed(connection->manager_window_id(), 0, toplevel.wm_id, bitmap);
    }
    dbgln("WindowServer: WM: X11 window {} icon {}x{}", toplevel.wm_id, toplevel.icon->width(), toplevel.icon->height());
}

X11Toplevel* find_x11_by_wm_id(Server& server, i32 wm_id)
{
    for (auto& toplevel : server.x11_toplevels) {
        if (toplevel->wm_id == wm_id)
            return toplevel.ptr();
    }
    return nullptr;
}

void tell_wms_applet_area_size_changed(Server&, Gfx::IntSize size)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_applet_area_size_changed(connection->manager_window_id(), size);
    }
}

bool perform_action(Server& server, Config::Action action)
{
    switch (action) {
    case Config::Action::SwitchWorkspaceLeft:
        return switch_direction(server, -1, 0, false);
    case Config::Action::SwitchWorkspaceRight:
        return switch_direction(server, 1, 0, false);
    case Config::Action::SwitchWorkspaceUp:
        return switch_direction(server, 0, -1, false);
    case Config::Action::SwitchWorkspaceDown:
        return switch_direction(server, 0, 1, false);
    case Config::Action::CarryWorkspaceLeft:
        return switch_direction(server, -1, 0, true);
    case Config::Action::CarryWorkspaceRight:
        return switch_direction(server, 1, 0, true);
    case Config::Action::CarryWorkspaceUp:
        return switch_direction(server, 0, -1, true);
    case Config::Action::CarryWorkspaceDown:
        return switch_direction(server, 0, 1, true);
    case Config::Action::TileLeft:
        return tile_focused(server, WindowTileType::Left);
    case Config::Action::TileRight:
        return tile_focused(server, WindowTileType::Right);
    case Config::Action::Maximize:
        return maximize_focused(server);
    case Config::Action::Minimize:
        return minimize_focused(server);
    case Config::Action::MaximizeHorizontal:
        return tile_focused(server, WindowTileType::HorizontallyMaximized);
    case Config::Action::MaximizeVertical:
        return tile_focused(server, WindowTileType::VerticallyMaximized);
    default:
        return false; // Lock is handled by the compositor (SessionLock.cpp).
    }
}

}
