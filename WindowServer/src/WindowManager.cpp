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

#include "WindowManagerServerDefaultStub.h"

// wlroots is a C library and does not wrap its headers in extern "C", so we do.
extern "C" {
#include <wlr/types/wlr_scene.h>
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
                wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
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
            wlr_scene_node_set_enabled(&toplevel->scene_tree->node, !minimized);
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
                    wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
                    changed = true;
                }
            } else if (toplevel->hidden_by_show_desktop) {
                toplevel->hidden_by_show_desktop = false;
                toplevel->minimized = false;
                wlr_scene_node_set_enabled(&toplevel->scene_tree->node, true);
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

    virtual void set_workspace(u32, u32) override { }
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
        connection->async_window_state_changed(connection->manager_window_id(), 0, toplevel.wm_id, 0, 0,
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
