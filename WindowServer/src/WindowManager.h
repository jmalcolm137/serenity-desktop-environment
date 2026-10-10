/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>

namespace Core {
class LocalSocket;
}

// The compositor's window manager: the Serenity WindowManagerServer the shell's
// Taskbar speaks. In SerenityOS the WindowServer serves this protocol itself, so
// do we - from the compositor's window state, with no bridging shim. The
// window-management policy (workspaces, tiling, the switcher) grows here with
// WS1/WS2; today it is the protocol plus the focus/minimise policy.
namespace WindowManager {

// Operations the window manager calls back into the compositor. main.cpp
// registers these at startup, so this module stays free of main.cpp's internals.
struct HostHooks {
    void (*focus_toplevel)(Server&, Toplevel*) { nullptr };
    void (*open_window_menu)(Server&, Toplevel&, Gfx::IntPoint) { nullptr };
};
void set_host_hooks(HostHooks);

// Accept a new Taskbar connection (its end of the WM portal).
void accept_connection(Server&, NonnullOwnPtr<Core::LocalSocket>);

// Report a window's state change / removal to every connected Taskbar.
void emit_window_state(Toplevel&);
void emit_window_removed(Toplevel&);

Toplevel* find_toplevel_by_wm_id(Server&, i32 wm_id);

}
