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
    Toplevel* carried = nullptr;
    if (carry_focused && server.seat->keyboard_state.focused_surface) {
        if (auto* focused = wlr_xdg_toplevel_try_from_wlr_surface(server.seat->keyboard_state.focused_surface)) {
            if (auto* content_tree = static_cast<wlr_scene_tree*>(focused->base->data)) {
                if (auto* frame_tree = content_tree->node.parent)
                    carried = static_cast<Toplevel*>(frame_tree->node.data);
            }
        }
    }

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
