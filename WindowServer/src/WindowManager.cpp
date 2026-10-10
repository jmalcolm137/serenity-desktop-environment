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
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Connection.h>
#include <WindowServer/WindowManagerClientEndpoint.h>
#include <WindowServer/WindowManagerServerEndpoint.h>
#include <WindowServer/WindowType.h>
#include <string.h>
#include <xkbcommon/xkbcommon.h>

#include "Config.h"
#include "WindowManagerServerDefaultStub.h"

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
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
        for (auto& toplevel : m_server.toplevels)
            emit_window_state(*toplevel);
    }

    virtual void set_active_window(i32, i32 window_id) override
    {
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            if (toplevel->minimized) {
                toplevel->minimized = false;
                update_toplevel_visibility(*toplevel);
            }
            if (g_hooks.focus_toplevel)
                g_hooks.focus_toplevel(m_server, toplevel);
            emit_window_state(*toplevel);
        }
    }

    virtual void set_window_minimized(i32, i32 window_id, bool minimized) override
    {
        if (auto* toplevel = find_toplevel_by_wm_id(m_server, window_id)) {
            toplevel->minimized = minimized;
            update_toplevel_visibility(*toplevel);
            emit_window_state(*toplevel);
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
                    toplevel->minimized = true;
                    update_toplevel_visibility(*toplevel);
                    changed = true;
                }
            } else if (toplevel->hidden_by_show_desktop) {
                toplevel->hidden_by_show_desktop = false;
                toplevel->minimized = false;
                update_toplevel_visibility(*toplevel);
                changed = true;
            }
            if (changed)
                emit_window_state(*toplevel);
        }
    }
    virtual void start_window_resize(i32, i32, i32) override { }
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
    // The Taskbar reports each button's rect for a minimise/restore animation the
    // compositor does not implement; deliberately not consumed (see known-gaps).
    virtual void set_window_taskbar_rect(i32, i32, Gfx::IntRect const&) override { }

    // Applets are layer surfaces positioned by the compositor, so the shell's
    // applet-area position no longer needs the WindowServer to lay them out.
    virtual void set_applet_area_position(Gfx::IntPoint) override { }

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

    Gfx::IntRect rect = area;
    uint32_t xdg_edges = 0;
    switch (type) {
    case WindowTileType::Left:
        rect.set_width(area.width() / 2);
        xdg_edges = WLR_EDGE_LEFT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
        break;
    case WindowTileType::Right:
        rect.set_x(area.x() + area.width() / 2);
        rect.set_width(area.width() - area.width() / 2);
        xdg_edges = WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
        break;
    case WindowTileType::Top:
        rect.set_height(area.height() / 2);
        xdg_edges = WLR_EDGE_TOP | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
        break;
    case WindowTileType::Bottom:
        rect.set_y(area.y() + area.height() / 2);
        rect.set_height(area.height() - area.height() / 2);
        xdg_edges = WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
        break;
    case WindowTileType::HorizontallyMaximized:
        xdg_edges = WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
        break;
    case WindowTileType::VerticallyMaximized:
        xdg_edges = WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
        break;
    case WindowTileType::Maximized:
        xdg_edges = WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
        break;
    default:
        break;
    }

    toplevel.tile_type = type;
    toplevel.maximized = type == WindowTileType::Maximized;
    apply_tile_geometry(toplevel, rect, xdg_edges);
    emit_window_state(toplevel);
    dbgln("WindowServer: tiled wm_id={} as {}", toplevel.wm_id, to_underlying(type));
}

bool handle_tile_key(Server& server, u32 keysym, u32 modifiers)
{
    // Plain Super (or Super+Alt); Ctrl/Shift combos belong to other bindings.
    if ((modifiers & WLR_MODIFIER_LOGO) == 0)
        return false;
    if ((modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_SHIFT)) != 0)
        return false;
    bool const with_alt = (modifiers & WLR_MODIFIER_ALT) != 0;

    auto* toplevel = focused_toplevel(server);
    if (!toplevel)
        return false;

    if (!with_alt && keysym == XKB_KEY_Up) {
        if (toplevel->maximized)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::Maximized);
        return true;
    }
    if (!with_alt && keysym == XKB_KEY_Down) {
        if (toplevel->maximized) {
            set_untiled(*toplevel);
        } else {
            toplevel->minimized = true;
            update_toplevel_visibility(*toplevel);
            emit_window_state(*toplevel);
        }
        return true;
    }
    if (!with_alt && keysym == XKB_KEY_Left) {
        if (toplevel->tile_type == WindowTileType::Left)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::Left);
        return true;
    }
    if (!with_alt && keysym == XKB_KEY_Right) {
        if (toplevel->tile_type == WindowTileType::Right)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::Right);
        return true;
    }
    if (with_alt && (keysym == XKB_KEY_Left || keysym == XKB_KEY_Right)) {
        if (toplevel->tile_type == WindowTileType::HorizontallyMaximized)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::HorizontallyMaximized);
        return true;
    }
    if (with_alt && (keysym == XKB_KEY_Up || keysym == XKB_KEY_Down)) {
        if (toplevel->tile_type == WindowTileType::VerticallyMaximized)
            set_untiled(*toplevel);
        else
            set_tiled(server, *toplevel, WindowTileType::VerticallyMaximized);
        return true;
    }
    return false;
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
    bool const on_current = toplevel.workspace_row == s_current_row && toplevel.workspace_column == s_current_column;
    wlr_scene_node_set_enabled(&toplevel.scene_tree->node, on_current && !toplevel.minimized);
}

void on_toplevel_mapped(Toplevel& toplevel)
{
    toplevel.workspace_row = s_current_row;
    toplevel.workspace_column = s_current_column;
    update_toplevel_visibility(toplevel);
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
    dbgln("WindowServer: workspaces {}x{}, current {},{}", s_rows, s_columns, s_current_row, s_current_column);
}

void switch_workspace(Server& server, u32 row, u32 column, bool carry_focused)
{
    if (row >= s_rows || column >= s_columns)
        return;
    if (row == s_current_row && column == s_current_column)
        return;

    // Carry the focused window into the target stack (Ctrl+Shift+Alt+arrow).
    Toplevel* carried = carry_focused ? focused_toplevel(server) : nullptr;

    s_current_row = row;
    s_current_column = column;
    if (carried) {
        carried->workspace_row = row;
        carried->workspace_column = column;
    }

    for (auto& toplevel : server.toplevels)
        update_toplevel_visibility(*toplevel);

    // Focus a visible window on the new stack so the keyboard follows.
    for (auto& toplevel : server.toplevels) {
        if (toplevel->workspace_row == row && toplevel->workspace_column == column && !toplevel->minimized) {
            if (g_hooks.focus_toplevel)
                g_hooks.focus_toplevel(server, toplevel.ptr());
            break;
        }
    }

    for (auto& toplevel : server.toplevels)
        emit_window_state(*toplevel);
    emit_workspace_changed(server);
    dbgln("WindowServer: switched to workspace {},{}", row, column);
}

bool handle_workspace_key(Server& server, u32 keysym, u32 modifiers)
{
    bool const ctrl = (modifiers & WLR_MODIFIER_CTRL) != 0;
    bool const alt = (modifiers & WLR_MODIFIER_ALT) != 0;
    if (!ctrl || !alt)
        return false;
    bool const carry = (modifiers & WLR_MODIFIER_SHIFT) != 0;

    u32 row = s_current_row;
    u32 column = s_current_column;
    switch (keysym) {
    case XKB_KEY_Left:
        if (column == 0)
            return true;
        column--;
        break;
    case XKB_KEY_Right:
        if (column + 1 >= s_columns)
            return true;
        column++;
        break;
    case XKB_KEY_Up:
        if (row == 0)
            return true;
        row--;
        break;
    case XKB_KEY_Down:
        if (row + 1 >= s_rows)
            return true;
        row++;
        break;
    default:
        return false;
    }

    switch_workspace(server, row, column, carry);
    return true;
}

bool handle_super_key(Server& server, u32 keysym, u32 modifiers, bool pressed)
{
    if ((modifiers & WLR_MODIFIER_LOGO) == 0)
        return false;
    if (!pressed)
        return true;

    auto emit = [&](auto&& call) {
        for (auto& connection : g_connections) {
            if (connection->manager_window_id() >= 0)
                call(*connection, connection->manager_window_id());
        }
    };

    switch (keysym) {
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
        emit([](auto& connection, i32 wm_id) { connection.async_super_key_pressed(wm_id); });
        return true;
    case XKB_KEY_space:
        emit([](auto& connection, i32 wm_id) { connection.async_super_space_key_pressed(wm_id); });
        return true;
    case XKB_KEY_d:
    case XKB_KEY_D:
        emit([](auto& connection, i32 wm_id) { connection.async_super_d_key_pressed(wm_id); });
        return true;
    default:
        break;
    }

    if (keysym >= XKB_KEY_1 && keysym <= XKB_KEY_9) {
        auto digit = static_cast<u8>(keysym - XKB_KEY_1 + 1);
        emit([digit](auto& connection, i32 wm_id) { connection.async_super_digit_key_pressed(wm_id, digit); });
        return true;
    }

    return false;
}

void emit_window_state(Toplevel& toplevel)
{
    StringView title;
    if (toplevel.xdg_toplevel->title)
        title = StringView { toplevel.xdg_toplevel->title, strlen(toplevel.xdg_toplevel->title) };
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() < 0)
            continue;
        // Chrome windows (the panel, applets, the desktop) are layer surfaces,
        // not toplevels, so nothing reported here is chrome.
        connection->async_window_state_changed(connection->manager_window_id(), 0, toplevel.wm_id, toplevel.workspace_row, toplevel.workspace_column,
            toplevel.frame_active && !toplevel.minimized, false, toplevel.minimized, false,
            static_cast<i32>(WindowServer::WindowType::Normal), ByteString { title },
            Gfx::IntRect { 0, 0, 0, 0 }, Optional<i32> { });
    }
}

void emit_window_removed(Toplevel& toplevel)
{
    for (auto& connection : g_connections) {
        if (connection->manager_window_id() >= 0)
            connection->async_window_removed(connection->manager_window_id(), 0, toplevel.wm_id);
    }
}

}
