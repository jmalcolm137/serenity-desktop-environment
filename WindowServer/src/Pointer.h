/*
 * Copyright (c) 2026, the Serenity Desktop Environment developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "Compositor.h"

Toplevel* toplevel_at(Server& server, double lx, double ly, wlr_surface** surface, double* sx, double* sy);
void reset_cursor_mode(Server& server);
void handle_touch_down(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly);
void handle_touch_motion(Server& server, uint32_t time_msec, int32_t touch_id, double lx, double ly);
void handle_touch_up(Server& server, uint32_t time_msec, int32_t touch_id);
void handle_touch_cancel(Server& server);
void dismiss_fragile_windows(Server& server, StringView reason);

void initialize_pointer(Server&);
