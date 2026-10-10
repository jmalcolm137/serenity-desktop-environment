/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "WindowManagerServer.h"
#include "WindowManagerServerConnection.h"
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Notifier.h>
#include <LibCore/PortalServer.h>
#include <LibCore/Socket.h>
#include <LibThreading/Thread.h>
#include <WindowServer/WindowType.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

namespace {

struct Toplevel {
    zwlr_foreign_toplevel_handle_v1* handle { nullptr };
    i32 window_id { 0 };
    ByteString title;
    ByteString app_id;
    bool active { false };
    bool minimized { false };
};

Vector<NonnullOwnPtr<Toplevel>> g_toplevels;
i32 g_next_window_id { 1 };
wl_display* g_display { nullptr };
wl_seat* g_seat { nullptr };
zwlr_foreign_toplevel_manager_v1* g_manager { nullptr };
RefPtr<Core::Notifier> g_notifier;
RefPtr<SDE::WindowManagerServerConnection> g_connection;
int g_server_fd { -1 };
int g_client_fd { -1 };
bool g_started { false };

Toplevel* find_toplevel(i32 window_id)
{
    for (auto& toplevel : g_toplevels) {
        if (toplevel->window_id == window_id)
            return toplevel.ptr();
    }
    return nullptr;
}

void emit_window_state_changed(Toplevel const& toplevel)
{
    dbgln("WindowManagerServer: state id={} title='{}' active={} minimized={}", toplevel.window_id, toplevel.title, toplevel.active, toplevel.minimized);
    if (!g_connection || g_connection->manager_window_id() < 0)
        return;
    // FIXME: plumb real geometry/skip-taskbar through the WindowServer.
    bool is_chrome = toplevel.app_id == "serenity-taskbar" || toplevel.app_id == "serenity-applet" || toplevel.app_id == "serenity-desktop";
    g_connection->async_window_state_changed(
        g_connection->manager_window_id(), 0, toplevel.window_id, 0, 0,
        toplevel.active, false, toplevel.minimized, is_chrome,
        static_cast<i32>(WindowServer::WindowType::Normal), toplevel.title,
        Gfx::IntRect { 0, 0, 0, 0 }, Optional<i32> { });
}

void remove_toplevel(Toplevel* toplevel)
{
    if (g_connection && g_connection->manager_window_id() >= 0)
        g_connection->async_window_removed(g_connection->manager_window_id(), 0, toplevel->window_id);
    for (size_t index = 0; index < g_toplevels.size(); ++index) {
        if (g_toplevels[index].ptr() == toplevel) {
            g_toplevels.remove(index);
            return;
        }
    }
}

// --- wlr-foreign-toplevel-management handle listeners ---------------------

void handle_title(void* data, zwlr_foreign_toplevel_handle_v1*, char const* title)
{
    static_cast<Toplevel*>(data)->title = title ? title : "";
}

void handle_app_id(void* data, zwlr_foreign_toplevel_handle_v1*, char const* app_id)
{
    static_cast<Toplevel*>(data)->app_id = app_id ? app_id : "";
}

void handle_output_enter(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) { }
void handle_output_leave(void*, zwlr_foreign_toplevel_handle_v1*, wl_output*) { }

void handle_state(void* data, zwlr_foreign_toplevel_handle_v1*, wl_array* state)
{
    auto* toplevel = static_cast<Toplevel*>(data);
    toplevel->active = false;
    toplevel->minimized = false;
    for (auto* entry = static_cast<uint32_t*>(state->data);
        reinterpret_cast<char*>(entry) < reinterpret_cast<char*>(state->data) + state->size; ++entry) {
        if (*entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            toplevel->active = true;
        else if (*entry == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED)
            toplevel->minimized = true;
    }
}

void handle_done(void* data, zwlr_foreign_toplevel_handle_v1*)
{
    emit_window_state_changed(*static_cast<Toplevel*>(data));
}

void handle_closed(void* data, zwlr_foreign_toplevel_handle_v1* handle)
{
    remove_toplevel(static_cast<Toplevel*>(data));
    zwlr_foreign_toplevel_handle_v1_destroy(handle);
}

void handle_parent(void*, zwlr_foreign_toplevel_handle_v1*, zwlr_foreign_toplevel_handle_v1*) { }

zwlr_foreign_toplevel_handle_v1_listener const s_handle_listener = {
    handle_title,
    handle_app_id,
    handle_output_enter,
    handle_output_leave,
    handle_state,
    handle_done,
    handle_closed,
    handle_parent,
};

void manager_toplevel(void*, zwlr_foreign_toplevel_manager_v1*, zwlr_foreign_toplevel_handle_v1* handle)
{
    auto toplevel = make<Toplevel>();
    toplevel->handle = handle;
    toplevel->window_id = g_next_window_id++;
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &s_handle_listener, toplevel.ptr());
    g_toplevels.append(move(toplevel));
    dbgln("WindowManagerServer: new toplevel id={}", g_toplevels.last()->window_id);
}

void manager_finished(void*, zwlr_foreign_toplevel_manager_v1*) { }

zwlr_foreign_toplevel_manager_v1_listener const s_manager_listener = {
    manager_toplevel,
    manager_finished,
};

// --- registry -------------------------------------------------------------

void registry_global(void*, wl_registry* registry, uint32_t name, char const* interface, uint32_t version)
{
    if (strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) == 0)
        g_manager = static_cast<zwlr_foreign_toplevel_manager_v1*>(wl_registry_bind(registry, name, &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3));
    else if (strcmp(interface, wl_seat_interface.name) == 0)
        g_seat = static_cast<wl_seat*>(wl_registry_bind(registry, name, &wl_seat_interface, version < 5 ? version : 5));
}

void registry_remove(void*, wl_registry*, uint32_t) { }

wl_registry_listener const s_registry_listener = {
    registry_global,
    registry_remove,
};

// --- server thread --------------------------------------------------------

void start_server_thread()
{
    int server_fd = g_server_fd;
    auto thread = Threading::Thread::try_create([server_fd]() -> intptr_t {
        Core::EventLoop loop;

        g_display = wl_display_connect(nullptr);
        if (g_display) {
            auto* registry = wl_display_get_registry(g_display);
            wl_registry_add_listener(registry, &s_registry_listener, nullptr);
            wl_display_roundtrip(g_display);
            if (g_manager)
                zwlr_foreign_toplevel_manager_v1_add_listener(g_manager, &s_manager_listener, nullptr);
            wl_display_roundtrip(g_display);

            g_notifier = Core::Notifier::construct(wl_display_get_fd(g_display), Core::Notifier::Type::Read);
            g_notifier->on_activation = [] {
                if (wl_display_dispatch(g_display) < 0)
                    wl_display_cancel_read(g_display);
            };
        } else {
            dbgln("WindowManagerServer: could not connect to the WindowServer");
        }

        if (auto socket = Core::LocalSocket::adopt_fd(server_fd); !socket.is_error())
            g_connection = SDE::WindowManagerServerConnection::create(socket.release_value());

        loop.exec();
        return 0;
    },
        "WindowManagerServer"sv);

    if (thread.is_error()) {
        dbgln("WindowManagerServer: failed to start the server thread");
        return;
    }
    thread.value()->start();
    thread.value()->detach();
}

Optional<NonnullOwnPtr<Core::LocalSocket>> make_portal(ByteString const& path)
{
    if (path != "/tmp/portal/wm"sv)
        return { };

    if (!g_started) {
        g_started = true;
        start_server_thread();
    }

    if (g_client_fd < 0)
        return { };

    auto socket = Core::LocalSocket::adopt_fd(g_client_fd);
    g_client_fd = -1;
    if (socket.is_error())
        return { };
    return socket.release_value();
}

}

namespace SDE {

void initialize()
{
    int fds[2] { };
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) < 0) {
        dbgln("WindowManagerServer: failed to create the /tmp/portal/wm socketpair");
        return;
    }
    g_server_fd = fds[0];
    g_client_fd = fds[1];

    Core::PortalServer::add_connector([](ByteString const& path) {
        return make_portal(path);
    });
}

NonnullRefPtr<WindowManagerServerConnection> WindowManagerServerConnection::create(NonnullOwnPtr<Core::LocalSocket> socket)
{
    return adopt_ref(*new WindowManagerServerConnection(move(socket)));
}

WindowManagerServerConnection::WindowManagerServerConnection(NonnullOwnPtr<Core::LocalSocket> socket)
    : IPC::Connection<WindowManagerServerEndpoint, WindowManagerClientEndpoint>(*this, move(socket))
    , WindowManagerClientEndpoint::Proxy<WindowManagerServerEndpoint>(*this, { })
{
}

void WindowManagerServerConnection::set_event_mask(u32 event_mask)
{
    m_event_mask = event_mask;
}

void WindowManagerServerConnection::set_manager_window(i32 window_id)
{
    m_manager_window_id = window_id;
    for (auto& toplevel : g_toplevels)
        emit_window_state_changed(*toplevel);
}

void WindowManagerServerConnection::set_active_window(i32, i32 window_id)
{
    if (auto* toplevel = find_toplevel(window_id); toplevel && g_seat)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, g_seat);
}

void WindowManagerServerConnection::set_window_minimized(i32, i32 window_id, bool minimized)
{
    auto* toplevel = find_toplevel(window_id);
    if (!toplevel)
        return;
    if (minimized)
        zwlr_foreign_toplevel_handle_v1_set_minimized(toplevel->handle);
    else if (g_seat)
        zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, g_seat);
}

void WindowManagerServerConnection::toggle_show_desktop() { }
void WindowManagerServerConnection::start_window_resize(i32, i32, i32) { }
void WindowManagerServerConnection::popup_window_menu(i32, i32, Gfx::IntPoint) { }
void WindowManagerServerConnection::set_window_taskbar_rect(i32, i32, Gfx::IntRect const&) { }
void WindowManagerServerConnection::set_applet_area_position(Gfx::IntPoint) { }
void WindowManagerServerConnection::set_workspace(u32, u32) { }
void WindowManagerServerConnection::set_keymap(ByteString const&) { }

}

namespace {

struct AutoInitializer {
    AutoInitializer() { SDE::initialize(); }
};
AutoInitializer s_auto_initializer;

}
