/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "WindowManagerServerDefaultStub.h"
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibCore/Socket.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Connection.h>
#include <WindowServer/WindowManagerClientEndpoint.h>
#include <WindowServer/WindowManagerServerEndpoint.h>

namespace SDE {

// The SerenityOS WindowManagerServer protocol, served in-process for the shell
// (the Taskbar). Window state is fed from the WindowServer via the
// wlr-foreign-toplevel-management protocol; activation/minimization requests are
// forwarded back the same way.
class WindowManagerServerConnection final
    : public IPC::Connection<WindowManagerServerEndpoint, WindowManagerClientEndpoint>
    , public WindowManagerServerDefaultStub
    , public WindowManagerClientEndpoint::Proxy<WindowManagerServerEndpoint> {
public:
    static NonnullRefPtr<WindowManagerServerConnection> create(NonnullOwnPtr<Core::LocalSocket> socket);

    i32 manager_window_id() const { return m_manager_window_id; }

    virtual void set_event_mask(u32 event_mask) override;
    virtual void set_manager_window(i32 window_id) override;
    virtual void set_active_window(i32 client_id, i32 window_id) override;
    virtual void set_window_minimized(i32 client_id, i32 window_id, bool minimized) override;
    virtual void toggle_show_desktop() override;
    virtual void start_window_resize(i32 client_id, i32 window_id, i32 resize_direction) override;
    virtual void popup_window_menu(i32 client_id, i32 window_id, Gfx::IntPoint screen_position) override;
    virtual void set_window_taskbar_rect(i32 client_id, i32 window_id, Gfx::IntRect const& rect) override;
    virtual void set_applet_area_position(Gfx::IntPoint position) override;
    virtual void set_workspace(u32 row, u32 column) override;
    virtual void set_keymap(ByteString const& keymap) override;

private:
    explicit WindowManagerServerConnection(NonnullOwnPtr<Core::LocalSocket> socket);

    i32 m_manager_window_id { -1 };
    u32 m_event_mask { 0 };
};

}
