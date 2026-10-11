/*
 * Copyright (c) 2026, Serenity Desktop Environment contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Model.h"

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Forward.h>

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
    // WS2: the primary output's usable area, and telling a client where we put it.
    Gfx::IntRect (*primary_usable_area)(Server&) { nullptr };
    void (*report_position)(Toplevel&) { nullptr };
    // WS2: begin an interactive move/resize (start_window_resize).
    void (*begin_interactive)(Toplevel&, CursorMode, uint32_t edges) { nullptr };
    // WS2: the current theme palette, for the geometry HUD.
    Gfx::Palette const& (*palette)(Server&) { nullptr };
};
void set_host_hooks(HostHooks);

// Accept a new Taskbar connection (its end of the WM portal).
void accept_connection(Server&, NonnullOwnPtr<Core::LocalSocket>);

// Report a window's state change / removal to every connected Taskbar.
void emit_window_state(Toplevel&);
void emit_window_removed(Toplevel&);

// Tell every Taskbar the applet-area size changed (WS4).
void tell_wms_applet_area_size_changed(Server&, Gfx::IntSize);

Toplevel* find_toplevel_by_wm_id(Server&, i32 wm_id);

// --- workspaces (WS1) -------------------------------------------------------
// A grid of window stacks (SerenityOS WindowManager); only the current stack is
// visible. The grid comes from [Workspaces] in WindowServer.ini.

// Read the grid from the config and apply it.
void load_workspaces(Server&);

u32 workspace_rows();
u32 workspace_columns();
u32 current_workspace_row();
u32 current_workspace_column();

// A newly mapped window joins the current stack.
void on_toplevel_mapped(Toplevel&);

// Switch to a stack (set_workspace, keybindings); optionally carry the focused
// window along.
void switch_workspace(Server&, u32 row, u32 column, bool carry_focused);

// Set a window's scene visibility from its stack and minimized state.
void update_toplevel_visibility(Toplevel&);

// Ctrl+Alt+arrow switches stacks; add Shift to carry the focused window. Returns
// true when the key was consumed.
bool handle_workspace_key(Server&, u32 keysym, u32 modifiers);

// Super-key shortcuts the Taskbar expects (WindowManagerClient.ipc):
// Super, Super+Space, Super+D and Super+digit.
bool handle_super_key(Server&, u32 keysym, u32 modifiers, bool pressed);

// Snapping (WS2): Super+arrows maximize/minimize/tile the focused window.
bool handle_tile_key(Server&, u32 keysym, u32 modifiers);

// WS2 drag-to-edge snapping: call while moving a window (updates the tile
// preview) and on release (tiles to the previewed target, if any).
void update_move_snap(Server&, Toplevel&);
void finish_move_snap(Server&, Toplevel&);

// WS2 geometry HUD (SerenityOS WindowGeometryOverlay): a small size readout shown
// while moving or resizing. The move path updates it from update_move_snap(); the
// resize path calls update_resize_hud() with the intended content rect. Hide it
// when the interactive grab ends.
void update_resize_hud(Server&, Toplevel&, Gfx::IntRect content_rect);
void hide_geometry_hud();

}
