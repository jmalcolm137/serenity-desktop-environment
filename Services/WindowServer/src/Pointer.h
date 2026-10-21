/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

Toplevel* toplevel_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy);
void reset_cursor_mode(Server& server);
// Interactive move/resize (GrabState). The shell that owns the window computes its
// geometry and starts the grab; the pointer drives it until reset_cursor_mode().
// For a move, the offsets are the cursor's position in the window; for a resize,
// its distance from the dragged edge, with the window's box and the edges.
void begin_grab(Server& server, Toplevel* toplevel, X11Toplevel* x11, CursorMode mode, double offset_x, double offset_y, wlr_box const& box = {}, uint32_t edges = 0);
// The window an interactive move is dragging, if any.
Toplevel* moving_toplevel(Server const& server);
// The press that would have started a frame drag was consumed elsewhere.
void cancel_frame_grab(Server& server);
// A window is going away: end or drop any grab on it.
void forget_grab_window(Server& server, Toplevel& toplevel);
void forget_grab_window(Server& server, X11Toplevel& toplevel);
void handle_touch_down(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly);
void handle_touch_motion(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly);
void handle_touch_up(Server& server, uint32_t time_msec, int32_t touch_id);
void handle_touch_cancel(Server& server);
void dismiss_fragile_windows(Server& server, StringView reason);

void initialize_pointer(Server&);

// Unhook the listeners initialize wired, before the objects they listen on go.
void finish_pointer(Server&);
