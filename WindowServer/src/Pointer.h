/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

Toplevel* toplevel_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy);
void reset_cursor_mode(Server& server);
void new_pointer_constraint(wl_listener* listener, void* data);
void cursor_motion(wl_listener* listener, void* data);
void cursor_motion_absolute(wl_listener* listener, void* data);
void handle_touch_down(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly);
void handle_touch_motion(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly);
void handle_touch_up(Server& server, uint32_t time_msec, int32_t touch_id);
void handle_touch_cancel(Server& server);
void cursor_touch_down(wl_listener* listener, void* data);
void cursor_touch_motion(wl_listener* listener, void* data);
void cursor_touch_up(wl_listener* listener, void* data);
void cursor_touch_cancel(wl_listener* listener, void* data);
void dismiss_fragile_windows(Server& server, StringView reason);
void cursor_button(wl_listener* listener, void* data);
void cursor_axis(wl_listener* listener, void* data);
void cursor_frame(wl_listener* listener, void*);
void seat_request_cursor(wl_listener* listener, void* data);
void seat_pointer_focus_change(wl_listener* listener, void* data);
void seat_request_set_selection(wl_listener* listener, void* data);
void seat_request_set_primary_selection(wl_listener* listener, void* data);
